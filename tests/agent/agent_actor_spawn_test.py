#!/usr/bin/env python3
"""Real supervisor/worker actor preparation and lifecycle replay; no external assets."""
import json
import os
from pathlib import Path
import sys
import struct
import tempfile

from agent_protocol_test import Agent, actor, check, properties, successful


GOOD = r'''
AUDIT={starts=0,destroys=0}
Good={health=100, starts=0,
 OnStart=function(self) self.starts=self.starts+1; AUDIT.starts=AUDIT.starts+1 end,
 OnDestroy=function(self)
  AUDIT.destroys=AUDIT.destroys+1
  local g=Actor.Find('source'):GetComponent('Good'); g.health=g.health-1
 end}
'''
CHILD = r'''
Child={starts=0,updates=0,start_frame=-1,first_update=-1,
 OnStart=function(self) self.starts=self.starts+1; self.start_frame=Application.GetFrame() end,
 OnUpdate=function(self)
  assert(self.starts==1, 'update before start or duplicate start')
  if self.updates==0 then self.first_update=Application.GetFrame() end
  self.updates=self.updates+1
 end}
'''
PROBE = r'''
SpawnProbe={checks=0,rejections=0,immediate=false,add_ok=false,starts=0,destroys=0,
 OnStart=function(self)
  local child=Actor.Instantiate('StartChild')
  assert(Actor.Find('StartChild')==child and child:GetComponent('Child').starts==0)
 end,
 OnUpdate=function(self)
  if Input.GetKeyDown('1') then
   local names={'BadDefault','MissingType','BadBody','BadParticles','MissingMap','MissingAnimation','MissingTarget','SharedTarget'}
   local good=Actor.Find('source'):GetComponent('Good')
   for _,name in ipairs(names) do
    local ok,err=pcall(function() Actor.Instantiate(name) end)
    assert(not ok and type(err)=='string' and #err>0, name..' unexpectedly accepted')
    assert(Actor.Find(name)==nil, name..' leaked Find result')
    assert(AUDIT.starts==1 and AUDIT.destroys==0 and good.health==100, name..' ran candidate callbacks')
    self.rejections=self.rejections+1
   end
   assert(not pcall(function() self.actor:AddComponent('Bad') end))
   assert(self.actor:GetComponent('Bad')==nil)
   self.rejections=self.rejections+1
  end
  if Input.GetKeyDown('v') then
   local child=Actor.Instantiate('UpdateChild')
   self.immediate=Actor.Find('UpdateChild')==child and child:GetComponent('Child').starts==0
  end
  if Input.GetKeyDown('d') then
   local doomed=Actor.Instantiate('Doomed')
   assert(doomed:GetComponent('Good').starts==0)
   Actor.Destroy(doomed)
   assert(Actor.Find('Doomed')==nil)
  end
  if Input.GetKeyDown('a') then
   local added=self.actor:AddComponent('Child')
   assert(added~=nil and added.starts==0)
   self.add_ok=true
  end
  if Input.GetKeyDown('r') then
   local repaired=Actor.Instantiate('BadBody')
   assert(repaired:GetComponent('Rigidbody')~=nil)
  end
  self.checks=self.checks+1
 end,
 OnLateUpdate=function(self) self.starts=AUDIT.starts; self.destroys=AUDIT.destroys end}
'''


def put(root, path, data):
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(data if isinstance(data, str) else json.dumps(data))


def template(root, name, components):
    put(root, 'actor_templates/' + name + '.template', {'name': name, 'components': components})


def fixture(root):
    put(root, 'game.config', {'initial_scene': 'main'})
    put(root, 'scenes/main.scene', {'actors': [{'uid': 1, 'name': 'source', 'components': {
        'good': {'type': 'Good'}, 'probe': {'type': 'SpawnProbe'}}}]})
    for name, code in {'Good': GOOD, 'Child': CHILD, 'SpawnProbe': PROBE,
                       'Bad': 'Bad={owned=Rigidbody()}',
                       'Death': "Death={OnDestroy=function(self) Actor.Instantiate('DeathChild') end}"}.items():
        put(root, 'component_types/' + name + '.lua', code)
    for name in ('StartChild', 'UpdateChild', 'DeathChild'):
        template(root, name, {'child': {'type': 'Child'}})
    template(root, 'Doomed', {'a_good': {'type': 'Good'}, 'z_death': {'type': 'Death'}})
    bad_components = {
        'BadDefault': {'type': 'Bad'}, 'MissingType': {'type': 'UnknownType'},
        'BadBody': {'type': 'Rigidbody', 'width': 0},
        'BadParticles': {'type': 'ParticleSystem', 'duration_frames': 0},
        'MissingMap': {'type': 'Tilemap', 'map_resource': 'absent.tilemap.json'},
        'MissingAnimation': {'type': 'SpriteAnimation', 'animation_resource': 'absent.animation.json'},
        'MissingTarget': {'type': 'SpriteAnimation', 'animation_resource': 'absent.animation.json', 'renderer_key': 'absent'},
        'SharedTarget': {'type': 'SpriteAnimation', 'animation_resource': 'absent.animation.json'},
    }
    image = root / 'images/dot.bmp'
    image.parent.mkdir(parents=True, exist_ok=True)
    image.write_bytes(struct.pack('<2sIHHI', b'BM', 58, 0, 0, 54) +
                      struct.pack('<IiiHHIIiiII', 40, 1, 1, 1, 24, 0, 4, 0, 0, 0, 0) + b'\xff\xff\xff\x00')
    put(root, 'animations/dot.animation.json', {
        'version': 1, 'image': 'dot.bmp', 'columns': 1, 'rows': 1,
        'tile_width_px': 1, 'tile_height_px': 1,
        'clips': [{'name': 'idle', 'loop': True, 'frames': [{'row': 1, 'column': 1, 'ticks': 2}]}]})
    bad_components['MissingTarget']['animation_resource'] = 'dot.animation.json'
    bad_components['SharedTarget']['animation_resource'] = 'dot.animation.json'
    for name, bad in bad_components.items():
        components = {'a_good': {'type': 'Good'}, 'z_bad': bad}
        if name in ('MissingAnimation', 'SharedTarget'):
            components['sprite'] = {'type': 'SpriteRenderer'}
        if name == 'SharedTarget':
            components['y_other'] = {'type': 'SpriteAnimation', 'animation_resource': 'dot.animation.json'}
        template(root, name, components)


def run(runtime):
    with tempfile.TemporaryDirectory(prefix='pocket-agent-spawn-') as directory:
        root = Path(directory)
        fixture(root)
        agent = Agent(runtime, str(root))
        try:
            initial = successful(agent, {'cmd': 'observe'})
            original_uid = actor(initial, 'source')['uid']
            one = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': []})
            check(properties(one, 'StartChild', 'Child')['starts'] == 0,
                  'spawn during OnStart must not recursively start in its creation frame')
            two = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': []})
            child = properties(two, 'StartChild', 'Child')
            check((child['starts'], child['updates'], child['start_frame'], child['first_update']) == (1, 1, 1, 1),
                  'OnStart-created child must start then update on following tick')
            before_ids = [(a['uid'], a['name']) for a in two['actors']]
            failed = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': ['1']})
            check(properties(failed, 'source', 'SpawnProbe')['rejections'] == 9, 'did not exercise complete failure matrix')
            for snapshot in (failed, successful(agent, {'cmd': 'step', 'frames': 2, 'keys': []})):
                check([(a['uid'], a['name']) for a in snapshot['actors']] == before_ids,
                      'rejected preparation published a partial actor')
                check(properties(snapshot, 'source', 'Good')['health'] == 100,
                      'rejected never-started candidate ran destructive OnDestroy')
                audit = properties(snapshot, 'source', 'SpawnProbe')
                check(audit['starts'] == 1 and audit['destroys'] == 0, 'rejection scheduled lifecycle callbacks')
                check(actor(snapshot, 'source')['uid'] == original_uid, 'failure replaced original identity')
            creation = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': ['v']})
            check(properties(creation, 'source', 'SpawnProbe')['immediate'], 'valid spawn not immediately accessible')
            check(properties(creation, 'UpdateChild', 'Child')['starts'] == 0, 'OnUpdate spawn started recursively')
            following = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': []})
            child = properties(following, 'UpdateChild', 'Child')
            check(child['starts'] == 1 and child['updates'] == 1 and child['start_frame'] == creation['frame']
                  and child['first_update'] == creation['frame'], 'OnUpdate child order incorrect')
            destroyed = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': ['d']})
            check(not any(a['name'] == 'Doomed' for a in destroyed['actors']), 'destroy-before-start actor survived')
            check(properties(destroyed, 'source', 'Good')['health'] == 99,
                  'published-before-start actor lost established OnDestroy callback')
            check(properties(destroyed, 'DeathChild', 'Child')['starts'] == 0,
                  'OnDestroy-created child started recursively')
            after_death = successful(agent, {'cmd': 'step', 'frames': 2, 'keys': []})
            child = properties(after_death, 'DeathChild', 'Child')
            check(child['starts'] == 1 and child['updates'] >= 1 and child['start_frame'] == destroyed['frame'],
                  'OnDestroy-created child failed deferred startup/update')
            added = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': ['a']})
            check(properties(added, 'source', 'SpawnProbe')['add_ok'], 'valid AddComponent after failure failed')
            check(properties(added, 'source', 'Child')['starts'] == 0, 'AddComponent started synchronously')
            added_next = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': []})
            check(properties(added_next, 'source', 'Child')['starts'] == 1, 'AddComponent did not start next tick')
            template(root, 'BadBody', {'a_good': {'type': 'Good'},
                                     'z_bad': {'type': 'Rigidbody', 'width': 1, 'gravity_scale': 0}})
            repaired = successful(agent, {'cmd': 'step', 'frames': 1, 'keys': ['r']})
            actor(repaired, 'BadBody')
            check(properties(repaired, 'BadBody', 'Good')['starts'] == 0, 'repair retry started synchronously')
            reset = successful(agent, {'cmd': 'reset'})
            check(reset == initial, 'reset must restore fresh original world')
            successful(agent, {'cmd': 'quit'})
        finally:
            agent.close()


if __name__ == '__main__':
    check(len(sys.argv) == 2, 'usage: agent_actor_spawn_test.py RUNTIME')
    run(os.path.abspath(sys.argv[1]))
    print('agent_actor_spawn_test: preparation rejection, lifecycle order, recovery and reset passed')
