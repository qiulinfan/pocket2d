#!/usr/bin/env python3
"""Scene.Load transactions and per-attempt errors through the real worker."""
import json
from pathlib import Path
import sys
import tempfile
from agent_savegame_test import SaveAgent
from agent_protocol_test import actor, check, properties, successful

PROBE = '''LOADS=(LOADS or 0)+1
if ARMED then
 local survivor=Actor.Find("survivor")
 if survivor then survivor:GetComponent("Probe").health=1;survivor:GetComponent("Rigidbody"):SetPosition(Vector2(9,9)) end
 local victim=Actor.Find("victim");if victim then Actor.Destroy(victim) end
 Save.Write("unexpected",1,{loads=LOADS});math.random()
end
Probe={starts=0,updates=0,health=3,loads=0,last_random=0,bag_count=0,events=0,identity_ok=false,
 OnStart=function(self)
  self.starts=self.starts+1;ARMED=true;self.body=self.actor:GetComponent("Rigidbody")
  self.bag=Inventory.Open("scene.items.json"):Create(2);self.bag:Add("herb",3,"")
  self.original_actor=self.actor;self.original_body=self.body;self.original_bag=self.bag
  Event.Subscribe("scene_ping",self,self.OnPing)
 end,
 OnPing=function(self,event) self.events=self.events+1 end,
 OnUpdate=function(self)
  self.updates=self.updates+1;self.loads=LOADS
  if Input.GetKeyDown("h") then self.health=self.health-1 end
  if Input.GetKeyDown("u") then self.bag:Remove(1,1) end
  if Input.GetKeyDown("r") then self.last_random=math.random(1,1000000) end
  if Input.GetKeyDown("p") then Event.Publish("scene_ping",{}) end
  if Input.GetKeyDown("l") then Scene.Load("bad") end
  if Input.GetKeyDown("v") then Scene.Load("other") end
  if Input.GetKeyDown("n") then Scene.Load("new") end
  if Input.GetKeyDown("z") then Scene.Load("zone/variant") end
  if Input.GetKeyDown("g") then Scene.Load("main") end
  self.body:SetVelocity(Vector2(Input.GetKey("d") and 1 or 0,0))
  self.bag_count=self.bag:GetSlot(1).count
  self.identity_ok=self.actor==self.original_actor and self.body==self.original_body and self.bag==self.original_bag and self.actor:IsValid()
 end,
 OnDestroy=function(self) DESTROYS=(DESTROYS or 0)+1 end}
'''


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def fixture(project):
    write(project / "game.config", json.dumps({"initial_scene": "main", "save_namespace": "scene_transition"}))
    write(project / "component_types/Probe.lua", PROBE)
    write(project / "component_types/Rigidbody.lua", "error('native-reserved file must not execute')")
    write(project / "items/scene.items.json", json.dumps({"version": 1, "items": [
        {"id": "herb", "display_name": "Herb", "kind": "material", "max_stack": 5}]}))
    scene = {"actors": [
        {"uid": 1, "name": "survivor", "components": {
            "probe": {"type": "Probe"}, "body": {"type": "Rigidbody", "gravity_scale": 0, "has_trigger": False}, "transform": {"type": "Transform"}}},
        {"uid": 2, "name": "victim", "components": {"transform": {"type": "Transform", "x": 3}}}]}
    write(project / "scenes/main.scene", json.dumps(scene))
    write(project / "scenes/other.scene", json.dumps(scene))
    write(project / "scenes/zone/variant.scene", json.dumps(scene))
    write(project / "scenes/bad.scene", json.dumps({"actors": [{"uid": 99, "name": "candidate", "components": {"missing": {"type": "MissingComponent"}}}]}))
    write(project / "scenes/new.scene", json.dumps({"actors": [
        {"uid": 50, "name": "new_a", "components": {"probe": {"type": "NewProbe"}}},
        {"uid": 51, "name": "new_b", "components": {"probe": {"type": "ZNewProbe"}}}]}))


def props(snapshot):
    return properties(snapshot, "survivor", "Probe")


def step(agent, frames=1, keys=()):
    return successful(agent, {"cmd": "step", "frames": frames, "keys": list(keys)})


def observe(agent):
    return successful(agent, {"cmd": "observe"})


def rejected_boundary(agent, keys=()):
    before = observe(agent)
    response = agent.request({"cmd": "step", "keys": list(keys)})
    check(not response["ok"] and response.get("code") == "scene_load_failed" and not response.get("requires_reset", False),
          "ordinary scene preflight failure was not recoverable: " + str(response))
    after = observe(agent)
    check(after["frame"] == before["frame"] + 1 and response["frame"] == after["frame"],
          "rejected queued transition must leave the old world running this one normal tick")
    check(after["scene"] == before["scene"] and
          [(a["uid"], a["name"]) for a in after["actors"]] == [(a["uid"], a["name"]) for a in before["actors"]],
          "failed transition replaced or destroyed old actors")
    for field in ["health", "starts", "bag_count", "loads", "identity_ok", "events"]:
        check(props(after)[field] == props(before)[field], "failed transition changed old live state: " + field)
    check(props(after)["updates"] == props(before)["updates"] + 1, "old world did not perform exactly one normal update")
    return after


def run(runtime, root):
    project = root / "project";fixture(project)
    source = project / "component_types/Probe.lua"
    control = SaveAgent(runtime, project, root / "control-saves")
    try:
        check(props(step(control))["loads"] == 1, "trusted startup evaluated source more than once")
        first = props(step(control, keys=["r"]))["last_random"]
        step(control)
        second = props(step(control, keys=["r"]))["last_random"]
    finally:
        control.close()
    agent = SaveAgent(runtime, project, root / "saves")
    try:
        check(props(step(agent))["loads"] == 1, "startup source count wrong")
        changed = step(agent, keys=["h", "u", "r"])
        check(props(changed)["health"] == 2 and props(changed)["bag_count"] == 2 and props(changed)["last_random"] == first,
              "could not prepare live state and deterministic RNG")
        pending = step(agent, keys=["l", "d"])
        before_x = actor(pending, "survivor")["position"]["x"]
        rejected = rejected_boundary(agent, ["d"])
        check(abs(actor(rejected, "survivor")["position"]["x"] - before_x - 1/60) < 0.00001,
              "failed preflight mutated physics beyond the normal old-world tick")
        continued = step(agent, 3, ["d", "r"])
        check(continued["frame"] == rejected["frame"] + 3 and props(continued)["updates"] == props(rejected)["updates"] + 3,
              "sticky old error interrupted a later legitimate three-frame request")
        check(props(continued)["last_random"] == second, "failed transition consumed the live RNG")
        check(not (root / "saves/scene_transition/unexpected.save.json").exists(), "scene preflight executed Save via top-level script")
        published = step(agent, keys=["p"])
        check(props(published)["events"] == 1 and props(published)["identity_ok"], "failed transition damaged named subscription/native handles")
        # New declarations are staged, never executed in the current live global environment.
        write(project / "component_types/NewProbe.lua", '''NewProbe={starts=0,value=10,loads=0,
          OnStart=function(self) self.starts=self.starts+1;self.loads=LOADS end,
          OnUpdate=function(self) if Input.GetKeyDown("g") then Scene.Load("main") end end}''')
        write(project / "component_types/ZNewProbe.lua", "error('late new declaration rejected')")
        step(agent, keys=["n"]);rejected_boundary(agent)
        registered = successful(agent, {"cmd": "describe"})["component_types"]
        check(not any(item["type"] in ["NewProbe", "ZNewProbe"] for item in registered),
              "failed declaration batch partially registered types")
        new_source = project / "component_types/NewProbe.lua"
        new_source.write_text(new_source.read_text().replace("value=10", "value=20"))
        write(project / "component_types/ZNewProbe.lua", "ZNewProbe={starts=0,OnStart=function(self) self.starts=self.starts+1 end}")
        step(agent, keys=["n"]);new_scene = step(agent)
        check(new_scene["scene"] == "new" and properties(new_scene, "new_a", "NewProbe")["starts"] == 1
              and properties(new_scene, "new_b", "ZNewProbe")["starts"] == 1
              and properties(new_scene, "new_a", "NewProbe")["loads"] == 1
              and properties(new_scene, "new_a", "NewProbe")["value"] == 20,
              "successful new declaration transition did not commit/start exactly once")
        # Editing an already loaded exact source path is not an implicit reload.
        source.write_text("Probe={")
        step(agent, keys=["g"]);cached = step(agent)
        check(cached["scene"] == "main" and props(cached)["loads"] == 1 and props(cached)["health"] == 3,
              "ordinary transition reexecuted edited cached source")
        before = observe(agent)
        explicit = agent.request({"cmd": "reload_scripts", "types": ["Probe"]})
        check(not explicit["ok"] and explicit.get("code") == "script_reload_failed" and observe(agent) == before,
              "explicit reload did not independently reject the edited syntax error")
        source.write_text(PROBE)
        # A new preferred same-stem path must be prepared in isolation and only
        # become the active prototype when the target scene commits.
        variant = project / "component_types/zone/Probe.lua"
        write(variant, "Probe={health=777};error('preferred variant rejected')")
        step(agent, keys=["z"]);rejected_boundary(agent)
        check(props(step(agent, 3))["loads"] == 1, "rejected preferred path changed old global source state")
        write(variant, '''Probe={starts=0,health=7,loads=0,
          OnStart=function(self) self.starts=self.starts+1;self.loads=LOADS end,
          OnUpdate=function(self) if Input.GetKeyDown("g") then Scene.Load("main") end end}''')
        step(agent, keys=["z"]);variant_scene = step(agent)
        check(props(variant_scene)["health"] == 7 and props(variant_scene)["starts"] == 1 and props(variant_scene)["loads"] == 1,
              "valid preferred path did not initialize its own prototype")
        step(agent, keys=["g"]);returned = step(agent)
        check(props(returned)["health"] == 3 and props(returned)["loads"] == 1,
              "returning to known root source reused wrong variant or reexecuted trusted source")
        reset = successful(agent, {"cmd": "reset"})
        check(reset["frame"] == 0, "reset did not create a new worker")
        check(props(step(agent))["loads"] == 1, "fresh reset repeated trusted source evaluation")
        check(props(step(agent, keys=["r"]))["last_random"] == first, "reset did not restore configured RNG seed")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def startup_preference(runtime, root):
    project = root / "project";fixture(project)
    write(project / "game.config", json.dumps({"initial_scene": "zone/variant", "save_namespace": "scene_transition"}))
    write(project / "component_types/Probe.lua", "error('root shadow must not execute for preferred initial scene')")
    write(project / "component_types/zone/Probe.lua", PROBE.replace("health=3", "health=7"))
    agent = SaveAgent(runtime, project, root / "saves")
    try:
        started = step(agent)
        check(props(started)["health"] == 7 and props(started)["loads"] == 1 and props(started)["starts"] == 1,
              "startup did not select the initial scene source before its single trusted evaluation")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


def teardown_failure(runtime, root):
    project = root / "project";fixture(project)
    source = project / "component_types/Probe.lua"
    source.write_text(PROBE.replace("DESTROYS=(DESTROYS or 0)+1", "error('teardown failure after commit began')"))
    agent = SaveAgent(runtime, project, root / "saves")
    try:
        step(agent);step(agent, keys=["v"])
        failed = agent.request({"cmd": "step", "keys": []})
        check(not failed["ok"] and failed.get("code") == "scene_load_failed" and failed.get("requires_reset") is True,
              "postcommit teardown error falsely claimed recoverable rollback: " + str(failed))
        unavailable = agent.request({"cmd": "observe"})
        check(not unavailable["ok"] and unavailable.get("requires_reset") is True,
              "fatal partially committed worker remained observable as healthy")
        source.write_text(PROBE)
        reset = successful(agent, {"cmd": "reset"})
        check(reset["frame"] == 0 and props(step(agent))["health"] == 3,
              "explicit reset did not recover after repaired teardown script")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: agent_scene_transition_test.py RUNTIME")
    with tempfile.TemporaryDirectory(prefix="pocket-agent-scene-transition-") as directory:
        root = Path(directory)
        run(Path(sys.argv[1]).resolve(), root / "normal")
        startup_preference(Path(sys.argv[1]).resolve(), root / "preferred-startup")
        teardown_failure(Path(sys.argv[1]).resolve(), root / "fatal")
    print("agent_scene_transition_test: isolated preflight, source cache/variants, per-attempt errors and fatal reset boundary passed")
