#!/usr/bin/env python3
"""Real agent workers reject bad native self receivers without poisoning gameplay.

Each negative case has its own project/worker so a crash cannot hide later cases.
No eval protocol, mocks, or synthetic observations are used. Optional --cases and
--evidence support bounded before-fix evidence; failures always return nonzero.
"""
import argparse
import json
import math
from pathlib import Path
import queue
import subprocess
import tempfile
import time
from agent_protocol_test import Agent, actor, check, properties

RECEIVERS = {'nil': 'nil', 'missing': '', 'number': '17', 'string': '"wrong"',
             'boolean': 'false', 'table': '{}', 'userdata': 'self.other'}
OPERATIONS = {
    'actor_get_name': ('self.get_name', ''),
    'body_set_velocity': ('self.set_velocity', 'Vector2(1,0)'),
    'body_set_gravity': ('self.set_gravity', '1'),
    'raw_index': ('self.index', '"x"'),
    'raw_newindex': ('self.newindex', '"x",9'),
}
CASES = {}
for operation, (function, arguments) in OPERATIONS.items():
    for receiver, expression in RECEIVERS.items():
        if operation.startswith('raw_') and receiver in ('table', 'userdata'):
            continue  # Generic metamethods may accept legal tables/other classes.
        call_arguments = '' if receiver == 'missing' else expression + (',' + arguments if arguments else '')
        CASES[operation + '_' + receiver] = function + '(' + call_arguments + ')'
CASES['actor_namespace_nil'] = 'Actor.GetName(nil)'
CASES['uncaught_body_set_velocity'] = 'self.body.SetVelocity(nil,Vector2(1,0))'
# A table is a legal generic receiver shape, but this unregistered metatable
# has no LuaBridge property-get/set dispatch tables. Reject it without asserting.
CASES['raw_index_empty_metatable_table'] = 'self.index(setmetatable({},{}),"x")'
CASES['raw_newindex_empty_metatable_table'] = 'self.newindex(setmetatable({},{}),"x",9)'

PROBE = r'''
ReceiverProbe={starts=0,ticks=0,attempts=0,caught=0,health=100,ready=false,
 identity_ok=false,nullable_ok=false,controls_ok=false,phase="initial",last_error="",
 namespace_get_name_type="unknown",
 OnStart=function(self)
  self.starts=self.starts+1
  self.target=Actor.Find('target');self.body=self.target:GetComponent('Rigidbody')
  self.anchor=Actor.Find('anchor');self.original_body=self.body;self.original_target=self.target
  self.other=Vector2(3,4)
  self.get_name=self.target.GetName;self.set_velocity=self.body.SetVelocity;self.set_gravity=self.body.SetGravityScale
  local meta=debug.getmetatable(self.body);self.index=meta.__index;self.newindex=meta.__newindex
  assert(type(self.get_name)=='function' and type(self.set_velocity)=='function' and type(self.set_gravity)=='function')
  assert(type(self.index)=='function' and type(self.newindex)=='function')
  self.namespace_get_name_type=type(Actor.GetName)
  @@NAMESPACE_REQUIREMENT@@
  -- These are legal receivers, not malformed arguments disguised as controls.
  assert(self.index(self.body,'x')==self.body.x)
  self.newindex(self.body,'x',self.body.x)
  local vector_meta=debug.getmetatable(self.other)
  assert(vector_meta.__index(self.other,'x')==3)
  vector_meta.__newindex(self.other,'x',5);assert(self.other.x==5)
  local namespace_meta=debug.getmetatable(Actor)
  assert(namespace_meta.__index(Actor,'receiver_absent_field')==nil)
  self.controls_ok=true
  -- Receiver is non-null, while the parent argument intentionally is nullable.
  assert(self.target:SetParent(nil));assert(self.anchor:GetChildCount()==0)
  self.nullable_ok=true;self.ready=true
 end,
 Snapshot=function(self)
  local b=self.body;local p=b:GetPosition();local v=b:GetVelocity()
  return {p.x,p.y,v.x,v.y,b:GetRotation(),b:GetAngularVelocity(),b:GetGravityScale(),
   b.x,b.y,b.rotation,b.gravity_scale,b.width,b.height,b.density,b.enabled,self.health,self.starts}
 end,
 OnUpdate=function(self)
  self.ticks=self.ticks+1
  if Input.GetKeyDown('t') then
   local before=self:Snapshot();self.attempts=self.attempts+1
   local ok,message=pcall(function() @@OPERATION@@ end)
   assert(not ok,'bad receiver unexpectedly accepted: @@CASE@@')
   assert(type(message)=='string' and #message>0,'bad receiver missing error: @@CASE@@')
   self.caught=self.caught+1;self.last_error=message;self.phase='rejected'
   local after=self:Snapshot()
   assert(#before==#after)
   for i,value in ipairs(before) do assert(value==after[i],'bad receiver changed live state: @@CASE@@') end
  end
  if Input.GetKeyDown('v') then
   assert(self.target:SetParent(self.anchor));assert(self.anchor:GetChildCount()==1)
   assert(self.target:SetParent(nil));assert(self.anchor:GetChildCount()==0)
   self.body:SetGravityScale(0);self.body:SetVelocity(Vector2(0.5,0));self.phase='valid_motion'
  end
  self.identity_ok=self.target:IsValid() and self.body.actor:IsValid()
   and self.target==self.original_target and rawequal(self.body,self.original_body)
   and self.body==self.target:GetComponent('Rigidbody') and self.target:GetName()=='target'
  assert(self.identity_ok,'receiver validation changed native/actor identity')
 end}
'''


UNCAUGHT_PROBE = r'''
ReceiverProbe={starts=0,ticks=0,attempts=0,health=100,ready=false,identity_ok=false,
 before_error=false,after_error=false,
 OnStart=function(self)
  self.starts=self.starts+1;self.target=Actor.Find('target')
  self.body=self.target:GetComponent('Rigidbody');self.original_body=self.body
  self.original_target=self.target;self.ready=true
 end,
 OnUpdate=function(self)
  self.ticks=self.ticks+1
  if not self.before_error then
   self.before_error=true;self.attempts=self.attempts+1
   -- Deliberately NOT wrapped in Lua pcall: the host must report this callback
   -- error while retaining the explicitly changed pre-error flag and tick.
   self.body.SetVelocity(nil,Vector2(1,0))
   self.after_error=true
  end
  self.identity_ok=self.target:IsValid() and self.body.actor:IsValid()
   and self.target==self.original_target and rawequal(self.body,self.original_body)
   and self.body==self.target:GetComponent('Rigidbody')
  assert(self.identity_ok)
  if Input.GetKeyDown('v') then self.body:SetVelocity(Vector2(0.5,0)) end
 end}
'''

class BoundedAgent(Agent):
    def __init__(self, runtime, project, deadline):
        self.deadline = deadline
        self.last_response = None
        super().__init__(str(runtime), str(project))

    def request(self, request, raw=False):
        remaining = self.deadline - time.monotonic()
        check(remaining > 0, 'overall receiver-test deadline reached')
        self.process.stdin.write((request if raw else json.dumps(request)) + '\n')
        self.process.stdin.flush()
        try:
            line = self.lines.get(timeout=min(3.0, remaining))
        except queue.Empty as error:
            raise AssertionError('bounded agent response timeout: ' + ''.join(self.errors[-5:])) from error
        check(line is not None, 'agent exited before response: ' + ''.join(self.errors[-5:]))
        response = json.loads(line)
        check(isinstance(response, dict) and isinstance(response.get('ok'), bool), 'invalid protocol response')
        self.last_response = response
        return response

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=0.4)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=0.4)


def fixture(project, case):
    (project / 'component_types').mkdir(parents=True)
    (project / 'scenes').mkdir()
    source = PROBE.replace('@@OPERATION@@', CASES[case]).replace('@@CASE@@', case)
    source = source.replace('@@NAMESPACE_REQUIREMENT@@',
                            "assert(type(Actor.GetName)=='function','Actor.GetName not exposed; not receiver coverage')"
                            if case == 'actor_namespace_nil' else '')
    (project / 'component_types/ReceiverProbe.lua').write_text(source)
    (project / 'game.config').write_text(json.dumps({'initial_scene': 'receivers'}))
    (project / 'scenes/receivers.scene').write_text(json.dumps({'actors': [
        {'uid': 1, 'name': 'guardian', 'components': {'probe': {'type': 'ReceiverProbe'}}},
        {'uid': 2, 'name': 'target', 'parent_uid': 3, 'components': {
            'body': {'type': 'Rigidbody', 'x': 2, 'y': 3, 'gravity_scale': 0,
                     'has_trigger': False, 'collider_type': 'circle', 'radius': 0.25},
            'transform': {'type': 'Transform', 'x': 2, 'y': 3}}},
        {'uid': 3, 'name': 'anchor', 'components': {'transform': {'type': 'Transform'}}},
    ]}))


def finite(value):
    if isinstance(value, float):
        check(math.isfinite(value), 'non-finite observation after bad receiver')
    elif isinstance(value, dict):
        for child in value.values():
            finite(child)
    elif isinstance(value, list):
        for child in value:
            finite(child)


def request(agent, command):
    response = agent.request(command)
    check(response['ok'], 'expected caught Lua error and live worker, got: ' + json.dumps(response))
    finite(response)
    return response


def stats(snapshot):
    return properties(snapshot, 'guardian', 'ReceiverProbe')


def step(agent, frames=1, keys=()):
    return request(agent, {'cmd': 'step', 'frames': frames, 'keys': list(keys)})


def run_uncaught(runtime, project, case, deadline):
    fixture(project, case)
    (project / 'component_types/ReceiverProbe.lua').write_text(UNCAUGHT_PROBE)
    agent = BoundedAgent(runtime, project, deadline)
    result = {'case': case, 'operation': CASES[case], 'status': 'failed', 'phase': 'uncaught_first_update'}
    try:
        initial = request(agent, {'cmd': 'observe'})
        identities = [(item['uid'], item['name']) for item in initial['actors']]
        target_before = actor(initial, 'target')
        failed = agent.request({'cmd': 'step', 'frames': 3, 'keys': []})
        # Ordinary callback errors historically have no explicit code field;
        # require the actual recoverable agent_errors policy, not an invented
        # wire value or whole-frame rollback.
        check(not failed['ok'] and not failed.get('requires_reset', False) and
              failed.get('code', 'script_error') == 'script_error' and
              'ReceiverProbe.lua' in failed.get('error', '') and failed.get('frame') == 1,
              'uncaught invalid receiver was not an ordinary completed-tick script error: ' + json.dumps(failed))
        observed = request(agent, {'cmd': 'observe'})
        check(observed['frame'] == 1 and observed['scene'] == initial['scene'] and
              [(item['uid'], item['name']) for item in observed['actors']] == identities and
              actor(observed, 'target') == target_before,
              'uncaught receiver error mutated the unrelated world or invalidated observation')
        state = stats(observed)
        check(state['ready'] and state['starts'] == 1 and state['ticks'] == 1 and
              state['attempts'] == 1 and state['before_error'] and not state['after_error'] and state['health'] == 100,
              'callback pre-error changes were rolled back or execution continued after failure')
        result['phase'] = 'continue_without_reset'
        continued = step(agent, frames=3)
        check(continued['frame'] == 4 and stats(continued)['ticks'] == 4 and
              stats(continued)['attempts'] == 1 and stats(continued)['identity_ok'] and
              actor(continued, 'target') == target_before,
              'old world could not complete a later full request without reset')
        valid = step(agent, keys=['v'])
        moved = step(agent, frames=2)
        check(actor(moved, 'target')['position']['x'] > actor(valid, 'target')['position']['x'] > 2,
              'valid native movement failed after uncaught receiver error')
        result['phase'] = 'reset_replay'
        check(request(agent, {'cmd': 'reset'}) == initial, 'uncaught-case reset did not restore fresh state')
        replay = agent.request({'cmd': 'step', 'frames': 3, 'keys': []})
        check(not replay['ok'] and not replay.get('requires_reset', False) and replay['frame'] == 1,
              'uncaught receiver reset replay changed the failure contract')
        replayed_world = request(agent, {'cmd': 'observe'})
        check(actor(replayed_world, 'target') == target_before and stats(replayed_world)['before_error'] and
              stats(replayed_world)['attempts'] == 1 and not stats(replayed_world)['after_error'],
              'reset replay lost pre-error state/native-world preservation')
        request(agent, {'cmd': 'quit'})
        result.update(status='passed', phase='complete', response=failed)
    except Exception as error:
        result['error'] = str(error)
        result['response'] = agent.last_response
        result['stderr_tail'] = ''.join(agent.errors[-10:])
    finally:
        agent.close()
    return result


def run_case(runtime, project, case, deadline):
    if case == 'uncaught_body_set_velocity':
        return run_uncaught(runtime, project, case, deadline)
    fixture(project, case)
    agent = BoundedAgent(runtime, project, deadline)
    result = {'case': case, 'operation': CASES[case], 'status': 'failed', 'phase': 'initial'}
    try:
        initial = request(agent, {'cmd': 'observe'})
        first = step(agent)
        result['namespace_get_name_type'] = stats(first)['namespace_get_name_type']
        check(stats(first)['ready'] and stats(first)['controls_ok'] and stats(first)['nullable_ok'] and
              stats(first)['starts'] == 1 and stats(first)['identity_ok'], 'native/nullable controls did not initialize')
        identities = [(item['uid'], item['name']) for item in first['actors']]
        target_before = actor(first, 'target')
        result['phase'] = 'negative_receiver'
        rejected = step(agent, keys=['t'])
        check(stats(rejected)['caught'] == 1 and stats(rejected)['attempts'] == 1 and
              stats(rejected)['phase'] == 'rejected', 'actual Lua pcall did not catch exactly one rejection')
        for observed in (rejected, request(agent, {'cmd': 'observe'}), step(agent, frames=3)):
            check(observed['scene'] == first['scene'] and
                  [(item['uid'], item['name']) for item in observed['actors']] == identities,
                  'bad receiver changed scene or actor identities')
            check(actor(observed, 'target') == target_before, 'bad receiver affected body/configuration or later physics')
            check(stats(observed)['health'] == 100 and stats(observed)['starts'] == 1 and
                  stats(observed)['caught'] == 1 and stats(observed)['identity_ok'],
                  'bad receiver restarted/changed native gameplay state')
        check(stats(observed)['ticks'] == 5 and observed['frame'] == 5, 'continued old-world steps did not complete')
        result['phase'] = 'valid_after_rejection'
        valid = step(agent, keys=['v'])
        moved = step(agent, frames=3)
        check(actor(moved, 'target')['position']['x'] > actor(valid, 'target')['position']['x'] > 2 and
              stats(moved)['identity_ok'] and stats(moved)['nullable_ok'],
              'valid receiver movement/nullable parent stopped working after rejection')
        result['phase'] = 'reset_replay'
        reset = request(agent, {'cmd': 'reset'})
        check(reset == initial, 'reset failed to restore fresh scene')
        first_again = step(agent)
        check(first_again == first, 'reset did not reproduce valid startup/nullable receiver controls')
        again = step(agent, keys=['t'])
        check(actor(again, 'target') == target_before and stats(again)['caught'] == 1 and
              stats(again)['attempts'] == 1 and stats(again)['identity_ok'], 'reset replay failed receiver rejection')
        request(agent, {'cmd': 'quit'})
        result.update(status='passed', phase='complete', rejection=stats(rejected)['last_error'])
    except Exception as error:
        result['error'] = str(error)
        result['response'] = agent.last_response
        result['stderr_tail'] = ''.join(agent.errors[-10:])
    finally:
        agent.close()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--cases', help='comma-separated bounded diagnostic subset; default runs every case')
    parser.add_argument('--evidence', type=Path, help='write actual per-case protocol evidence to an ignored directory')
    args = parser.parse_args()
    cases = args.cases.split(',') if args.cases else list(CASES)
    check(cases and len(cases) == len(set(cases)) and all(case in CASES for case in cases), 'unknown/duplicate receiver case')
    runtime = args.runtime.resolve()
    deadline = time.monotonic() + 72
    results = []
    with tempfile.TemporaryDirectory(prefix='pocket-receiver-') as directory:
        for case in cases:
            if time.monotonic() >= deadline:
                results.append({'case': case, 'status': 'failed', 'error': 'overall deadline before case'})
                break
            result = run_case(runtime, Path(directory) / case, case, deadline)
            results.append(result)
            print(case + ': ' + result['status'] + (' (' + result['phase'] + ')' if 'phase' in result else ''), flush=True)
    if args.evidence:
        args.evidence.mkdir(parents=True, exist_ok=True)
        (args.evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    failures = [result for result in results if result['status'] != 'passed']
    for failure in failures:
        print(failure['case'] + ': ' + failure['error'], flush=True)
    check(not failures and len(results) == len(cases),
          f'{len(failures)} receiver cases failed; {len(results)}/{len(cases)} completed')
    print(f'agent_receiver_test: {len(results)} isolated receiver cases, continued world and reset replay passed')


if __name__ == '__main__':
    main()
