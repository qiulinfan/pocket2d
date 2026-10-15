#!/usr/bin/env python3
"""Executable Lua getters must not invalidate native animation event dispatch."""
import json
from pathlib import Path
import shutil
import sys
import tempfile

from agent_protocol_test import Agent, actor, check, properties, successful


ACTIONS = {
    "add": 'for i=1,32 do owner:AddComponent("AddedProbe") end',
    "stop": "animation:Stop()",
    "restart": 'animation:Restart("walk")',
    "remove": "owner:RemoveComponent(animation)",
    "destroy": "Actor.Destroy(owner)",
    "error": 'error("intentional-animation-getter-error")',
    "enabled_stop": "animation:Stop()",
}


def probe_source(case):
    return '''local owner, animation, gauge, probe
local fired=false
local function action()
    if fired then return end
    fired=true
    %s
end
GetterProbe=setmetatable({
    OnStart=function(self)
        owner=self.actor;probe=self
        animation=owner:GetComponentByKey("anim")
        gauge=Actor.Find("view"):GetComponent("AnimationView")
        gauge.getter_calls=0;gauge.getter_deliveries=0
        local controller=owner:GetComponent("AnimationController")
        controller.auto_select=false;controller.mode="external"
    end
}, {__index=function(_,key)
    if key=="OnAnimationEvent" and owner then
        gauge.getter_calls=gauge.getter_calls+1
        %s
        return function(self,event)
            gauge.getter_deliveries=gauge.getter_deliveries+1
        end
    elseif key=="enabled" and owner then
        action()
        return true
    end
end})
''' % (ACTIONS[case], 'rawset(probe,"enabled",nil)' if case == "enabled_stop" else "action()")


def observer(snapshot, key):
    return next(component["properties"] for component in actor(snapshot, "player")["components"]
                if component["key"] == key)


def run_case(executable, source, case):
    with tempfile.TemporaryDirectory(prefix="pocket-animation-getter-") as directory:
        project = Path(directory) / "project"
        shutil.copytree(source, project)
        scene = project / "scenes/animation.scene"
        data = json.loads(scene.read_text())
        player = next(item for item in data["actors"] if item["name"] == "player")
        player["components"]["m_getter"] = {"type": "GetterProbe"}
        scene.write_text(json.dumps(data))
        (project / "component_types/GetterProbe.lua").write_text(probe_source(case))
        (project / "component_types/AddedProbe.lua").write_text("AddedProbe={}\n")
        agent = Agent(executable, str(project))
        try:
            initial = successful(agent, {"cmd": "observe"})
            response = agent.request({"cmd": "step"})
            if case == "error":
                check(not response["ok"] and "intentional-animation-getter-error" in response.get("error", ""),
                      "getter exception must return the Lua diagnostic: " + str(response))
                check(response.get("code") != "worker_exit" and not response.get("requires_reset", False),
                      "ordinary getter error must not abort or poison the worker")
                snapshot = successful(agent, {"cmd": "observe"})
                check(snapshot["frame"] == 1, "failed callback must leave the completed tick observable")
            else:
                check(response["ok"], case + " getter failed: " + str(response))
                snapshot = response
            gauge = properties(snapshot, "view", "AnimationView")
            check(gauge["getter_calls"] == 1, case + " did not execute exactly one callback getter")
            check(gauge["getter_deliveries"] == (1 if case == "add" else 0),
                  case + " dispatched an invalidated event to its getter recipient")
            if case == "destroy":
                check(not any(item["name"] == "player" for item in snapshot["actors"]),
                      "getter must actually destroy its actor")
            else:
                check(observer(snapshot, "a_observer")["frame_events"] == 1,
                      "recipient before getter must receive its event")
                check(observer(snapshot, "z_observer")["frame_events"] == (1 if case in ("add", "error") else 0),
                      case + " incorrectly retained or dropped a later recipient")
                if case == "add":
                    check(sum(c["type"] == "AddedProbe" for c in actor(snapshot, "player")["components"]) == 32,
                          "getter must actually grow the component container")
                elif case in ("stop", "enabled_stop"):
                    animation = properties(snapshot, "player", "SpriteAnimation")
                    check(animation["frame"] == 0 and not animation["playing"], "getter Stop did not take effect")
                elif case == "remove":
                    check(not any(c["type"] == "SpriteAnimation" for c in actor(snapshot, "player")["components"]),
                          "getter must actually remove the animation")
                elif case == "restart":
                    animation = properties(snapshot, "player", "SpriteAnimation")
                    check((animation["current_clip"], animation["frame"], animation["elapsed_ticks"]) == ("walk", 1, 0),
                          "getter Restart must preserve a fresh playback timeline")
                    next_tick = successful(agent, {"cmd": "step"})
                    check(properties(next_tick, "view", "AnimationView")["getter_deliveries"] == 1,
                          "new playback event must be deliverable on the following tick")
            check(successful(agent, {"cmd": "reset"}) == initial, case + " did not reset to its initial state")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, case + " quit failed")
            check(agent.lines.get(timeout=5) is None, case + " emitted unsolicited stdout")
        finally:
            agent.close()


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: animation_getter_reentry_test.py RUNTIME ANIMATION_FIXTURE")
    for case in ACTIONS:
        run_case(str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve(), case)
    print("animation_getter_reentry_test: mutation, playback invalidation, protected getters and reset passed")
