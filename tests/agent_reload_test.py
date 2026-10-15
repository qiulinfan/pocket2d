#!/usr/bin/env python3
"""Reload disk scripts in a live supervisor/worker; never simulate reload via reset."""
import json
from pathlib import Path
import sys
import tempfile
from agent_savegame_test import SaveAgent
from agent_protocol_test import actor, check, properties, successful


def probe(version, *, update=True, event=True):
    extra = "extra={hits=0},extra_hits=0,new_value=22," if version >= 2 else ""
    subscription = 'Event.Subscribe("reload_ping",self,self.OnPing)' if event else ""
    event_method = f'''OnPing=function(self,payload)
        self.events=self.events+{version};self.last_event_version={version}
    end,''' if event else ""
    update_method = f'''OnUpdate=function(self)
        self.updates=self.updates+{version};self.version_seen={version}
        local alpha=self.actor:GetName()=="alpha"
        if alpha and Input.GetKeyDown("h") then self.health=math.max(0,self.health-1) end
        if alpha and Input.GetKeyDown("u") then self.bag:Remove(1,1) end
        if alpha and Input.GetKeyDown("c") then self.cached_method=self.Adjust end
        if alpha and Input.GetKeyDown("v") then self.cached_method=nil end
        if alpha and Input.GetKeyDown("a") and not self.anonymous_added then
            self.anonymous_added=true
            Event.Subscribe("reload_anon",self,function(owner,payload) owner.events=owner.events+100 end)
        end
        self.body:SetVelocity(Vector2(alpha and Input.GetKey("d") and {version} or 0,0))
        {('if alpha and Input.GetKeyDown("m") then self.extra.hits=self.extra.hits+1 end;self.extra_hits=self.extra.hits' if version >= 2 else '')}
        self:Publish()
    end,''' if update else ""
    return f'''ReloadProbe={{
        starts=0,destroys=0,updates=0,health={3 if version == 1 else 9},events=0,last_event_version=0,
        version_seen=0,helper_version=0,nested_value=0,bag_count=0,bag_revision=0,identities_ok=false,
        anonymous_added=false,{extra}
        OnStart=function(self)
            self.starts=self.starts+1;self.health=self.health
            self.memory={{data={{coins=11}}}}
            self.body=self.actor:GetComponent("Rigidbody")
            self.catalog=Inventory.Open("reload.items.json");self.bag=self.catalog:Create(2)
            assert(self.bag:Add("herb",3,"").moved==3)
            self.original_actor=self.actor;self.original_body=self.body;self.original_bag=self.bag
            {subscription}
            self:Publish()
        end,
        OnDestroy=function(self) self.destroys=self.destroys+1 end,
        Publish=function(self)
            self.nested_value=self.memory.data.coins
            self.bag_count=self.bag:GetSlot(1).count;self.bag_revision=self.bag:GetRevision()
            self.identities_ok=self.actor==self.original_actor and self.body==self.original_body
                and self.bag==self.original_bag and self.actor:IsValid()
        end,
        Adjust=function(self) self.memory.data.coins=self.memory.data.coins+{version};self.helper_version={version};self:Publish() end,
        {event_method}
        {update_method}
    }}\n'''


DRIVER = '''ZDriver={ticks=0,starts=0,global_clean=true,
 OnStart=function(self) self.starts=self.starts+1 end,
 OnUpdate=function(self)
  self.ticks=self.ticks+1;self.global_clean=_G.ReloadLeak==nil
  if Input.GetKeyDown("p") then Event.Publish("reload_ping",{amount=1}) end
  if Input.GetKeyDown("j") then Actor.Find("alpha"):GetComponent("ReloadProbe"):Adjust() end
  if Input.GetKeyDown("n") then Actor.Instantiate("ReloadActor") end
 end}
'''


def fixture(root):
    for directory in ["component_types", "scenes", "items", "actor_templates"]:
        (root / directory).mkdir(parents=True)
    (root / "game.config").write_text(json.dumps({"game_title": "Reload verification", "initial_scene": "reload", "save_namespace": "script_reload"}))
    (root / "component_types/ReloadProbe.lua").write_text(probe(1))
    (root / "component_types/ZDriver.lua").write_text(DRIVER)
    (root / "items/reload.items.json").write_text(json.dumps({"version": 1, "items": [
        {"id": "herb", "display_name": "Herb", "kind": "material", "max_stack": 10}]}))
    def components(y):
        return {"body": {"type": "Rigidbody", "gravity_scale": 0, "y": y, "has_trigger": False},
                "logic": {"type": "ReloadProbe"}, "transform": {"type": "Transform", "y": y}}
    (root / "scenes/reload.scene").write_text(json.dumps({"actors": [
        {"uid": 1, "name": "alpha", "components": components(0)},
        {"uid": 2, "name": "beta", "components": components(3)},
        {"uid": 3, "name": "driver", "components": {"driver": {"type": "ZDriver"}}}]}))
    (root / "actor_templates/ReloadActor.template").write_text(json.dumps({"name": "gamma", "components": components(6)}))


def props(snapshot, name="alpha"):
    return properties(snapshot, name, "ReloadProbe")


def step(agent, frames=1, keys=()):
    return successful(agent, {"cmd": "step", "frames": frames, "keys": list(keys)})


def observe(agent):
    return successful(agent, {"cmd": "observe"})


def reload(agent, types=("ReloadProbe",)):
    return agent.request({"cmd": "reload_scripts", "types": list(types)})


def good_reload(agent, types=("ReloadProbe",), instances=2):
    before = observe(agent)
    result = reload(agent, types)
    check(result["ok"], "reload failed: " + str(result))
    check(result["frame"] == before["frame"] and result["reload"]["instances_updated"] == instances,
          "reload advanced simulation or reported wrong live-instance count")
    check(set(result["reload"]["types"]) == set(types) and result["reload"]["diagnostics"] == [],
          "successful reload has wrong structured metadata")
    check([(a["uid"], a["name"], a.get("position")) for a in result["actors"]] ==
          [(a["uid"], a["name"], a.get("position")) for a in before["actors"]], "reload replaced actors or moved physics")
    for previous in before["actors"]:
        if any(component["type"] == "ReloadProbe" for component in previous["components"]):
            for field in ["starts", "destroys", "health", "bag_count", "bag_revision", "nested_value", "identities_ok"]:
                check(props(result, previous["name"])[field] == props(before, previous["name"])[field],
                      "successful reload changed existing state/lifecycle field " + field)
    return result


def rejected_reload(agent, *, code="script_reload_failed", types=("ReloadProbe",)):
    before = observe(agent)
    result = reload(agent, types)
    check(result["ok"] is False and result.get("code") == code and not result.get("requires_reset", False),
          "ordinary reload failure poisoned the live worker: " + str(result))
    check(result["frame"] == before["frame"] and observe(agent) == before,
          "failed reload changed live world, properties or frame")
    if code == "script_reload_failed":
        check(result.get("diagnostics"), "reload failure omitted structured diagnostics")
        for diagnostic in result["diagnostics"]:
            check(all(key in diagnostic for key in ["type", "resource", "stage", "message", "line"])
                  and isinstance(diagnostic["line"], int), "diagnostic schema mismatch")
            for field in ["type", "resource", "stage", "message"]:
                check(isinstance(diagnostic[field], str) and len(diagnostic[field].encode("utf-8")) <= 4096,
                      "reload diagnostic exceeded its UTF-8 byte bound: " + field)
        check(len(result["error"].encode("utf-8")) <= 4096, "reload summary exceeded diagnostic byte bound")
    return result


def run(runtime, root):
    project = root / "project";fixture(project)
    source = project / "component_types/ReloadProbe.lua"
    driver = project / "component_types/ZDriver.lua"
    agent = SaveAgent(runtime, project, root / "saves")
    try:
        description = successful(agent, {"cmd": "describe"})
        check("reload_scripts" in description["commands"] and "reload_scripts" in description,
              "describe did not advertise reload contract")
        for request in [
            {"cmd": "reload_scripts"}, {"cmd": "reload_scripts", "types": []},
            {"cmd": "reload_scripts", "types": "ReloadProbe"},
            {"cmd": "reload_scripts", "types": [1]}, {"cmd": "reload_scripts", "types": [""]},
            {"cmd": "reload_scripts", "types": ["ReloadProbe", "ReloadProbe"]},
            {"cmd": "reload_scripts", "types": ["ReloadProbe\0other"]},
            {"cmd": "reload_scripts", "types": [str(i) for i in range(65)]},
        ]:
            before = observe(agent);bad = agent.request(request)
            check(not bad["ok"] and bad.get("code") == "invalid_argument" and observe(agent) == before,
                  "malformed reload request mutated or poisoned the worker")
        rejected_reload(agent, types=["Missing"]);rejected_reload(agent, types=["Rigidbody"])
        step(agent)
        step(agent, keys=["h", "u", "j"])
        live = step(agent, 5, ["d"])
        check(props(live)["health"] == 2 and props(live)["bag_count"] == 2 and props(live)["nested_value"] == 12,
              "did not prepare actual non-default live state")
        old = {name: props(live, name).copy() for name in ["alpha", "beta"]}
        source.write_text(probe(2))
        updated = good_reload(agent)
        for name in old:
            for field in ["starts", "destroys", "updates", "health", "bag_count", "bag_revision", "nested_value", "identities_ok"]:
                check(props(updated, name)[field] == old[name][field], "reload reset live field " + field)
            check(props(updated, name)["extra_hits"] == 0, "new default missing")
        advanced = step(agent, keys=["d", "m", "j"])
        check(props(advanced)["updates"] == old["alpha"]["updates"] + 2 and props(advanced)["version_seen"] == 2,
              "new OnUpdate did not replace cached lifecycle callback")
        check(props(advanced)["nested_value"] == 14 and props(advanced)["helper_version"] == 2,
              "ordinary prototype method did not update against live nested data")
        check(props(advanced)["extra_hits"] == 1 and props(advanced, "beta")["extra_hits"] == 0,
              "new table default aliases different live instances")
        check(abs(actor(advanced, "alpha")["position"]["x"] - actor(updated, "alpha")["position"]["x"] - 2 / 60) < 0.00001,
              "retained Rigidbody did not execute updated movement")
        consumed = step(agent, keys=["u"])
        check(props(consumed)["bag_count"] == 1 and props(consumed)["identities_ok"], "retained native inventory userdata is unusable")
        before_events = props(consumed)["events"]
        published = step(agent, keys=["p"])
        check(props(published)["events"] == before_events + 2 and props(published)["last_event_version"] == 2,
              "event subscription invoked old handler or duplicated new handler")
        step(agent, keys=["n"]);spawned = step(agent)
        check(props(spawned, "gamma")["starts"] == 1 and props(spawned, "gamma")["health"] == 9
              and props(spawned, "gamma")["version_seen"] == 2, "new actor did not use new prototype/defaults")
        # A later source failure must not publish an earlier staged candidate.
        source.write_text(probe(3));driver.write_text('error("late batch failure")')
        rejected_reload(agent, types=["ZDriver", "ReloadProbe"])
        before = observe(agent);after = step(agent)
        check(props(after)["updates"] == props(before)["updates"] + 2 and
              properties(after, "driver", "ZDriver")["ticks"] == properties(before, "driver", "ZDriver")["ticks"] + 1,
              "failed batch partially installed a method")
        driver.write_text(DRIVER)
        for bad_source in [
            "ReloadProbe={", 'error("candidate evaluation rejected")', "NotTheExport={}",
            'ReloadLeak=1;error("leak check")',
            'Actor.Destroy(Actor.Find("alpha"));ReloadProbe={}',
            'Save.Write("forbidden",1,{value=1});ReloadProbe={}',
            'Inventory.Open("reload.items.json");ReloadProbe={}',
            'ReloadProbe=setmetatable({}, {__index=function() error("do not execute") end})',
            'error(string.rep("x",20*1024*1024))', 'error(string.char(255))',
            'error(string.rep("a",10*1024*1024))', 'error(string.rep("汉",2000))',
            "while true do end", 'local huge=string.rep("x",40*1024*1024);ReloadProbe={}',
        ]:
            source.write_text(bad_source)
            rejected = rejected_reload(agent)
            if "10*1024*1024" in bad_source or '"汉",2000' in bad_source:
                check(any("[truncated]" in diagnostic["message"] for diagnostic in rejected["diagnostics"]),
                      "large error case did not actually exercise diagnostic truncation")
            alive = step(agent)
            check(props(alive)["version_seen"] == 2 and props(alive)["starts"] == 1 and props(alive)["identities_ok"],
                  "failed staging left old gameplay unusable")
            check(properties(alive, "driver", "ZDriver")["global_clean"], "candidate globals escaped staging")
            check(not (root / "saves/script_reload/forbidden.save.json").exists(), "candidate top-level host API wrote a save")
        # Fix and retry succeeds in the same worker after instruction/OOM staging failures.
        source.write_text(probe(3, update=False))
        removed = good_reload(agent, instances=3)
        before_updates = {name: props(removed, name)["updates"] for name in ["alpha", "beta", "gamma"]}
        stepped = step(agent, 3)
        check(all(props(stepped, name)["updates"] == count for name, count in before_updates.items()),
              "removed OnUpdate remained in cached update flags")
        before_events = props(stepped)["events"]
        published = step(agent, keys=["p"])
        check(props(published)["events"] == before_events + 3, "event callback did not update while OnUpdate absent")
        source.write_text(probe(2, event=False));removed = good_reload(agent, instances=3)
        before_events = props(removed)["events"]
        step(agent);published = step(agent, keys=["p"])
        check(props(published)["events"] == before_events, "deleted prototype event method remained subscribed")
        before_updates = props(removed)["updates"]
        check(props(published)["updates"] == before_updates + 4, "re-added OnUpdate not scheduled")
        # A cached function inside live state is an explicit unsupported migration.
        step(agent, keys=["c"]);source.write_text(probe(3, event=False))
        rejected_reload(agent)
        step(agent, keys=["v"]);good_reload(agent, instances=3)
        # Anonymous event subscriptions cannot be safely remapped by method name.
        step(agent, keys=["a"]);source.write_text(probe(2))
        rejected_reload(agent)
        reset = successful(agent, {"cmd": "reset"})
        check(reset["frame"] == 0 and len(reset["actors"]) == 3 and props(reset)["health"] == 9,
              "reset failed to start a fresh VM from repaired disk V2")
        fresh = step(agent)
        check(props(fresh)["starts"] == 1 and props(fresh)["updates"] == 2 and props(fresh)["bag_count"] == 3,
              "reset retained old runtime inventory, starts or data")
        before_events = props(fresh)["events"]
        published = step(agent, keys=["p"])
        check(props(published)["events"] == before_events + 2, "reset retained stale/anonymous subscriptions")
        successful(agent, {"cmd": "quit"})
    finally:
        agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: agent_reload_test.py RUNTIME")
    with tempfile.TemporaryDirectory(prefix="pocket-script-reload-") as directory:
        run(Path(sys.argv[1]).resolve(), Path(directory))
    print("agent_reload_test: live state/native handles, method/event replacement, atomic failure, budgets and reset passed")
