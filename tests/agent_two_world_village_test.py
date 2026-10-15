#!/usr/bin/env python3
"""Integrated village gameplay through the real supervised runtime and player input."""
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile

from agent_protocol_test import actor, check, properties, successful
from agent_savegame_test import SaveAgent


def state(snapshot):
    return properties(snapshot, "village", "VillageState")


class Replay:
    def __init__(self, runtime, project, saves):
        self.agent = SaveAgent(runtime, project, saves)
        self.commands = 0

    def request(self, command):
        self.commands += 1
        return successful(self.agent, command)

    def observe(self):
        return self.request({"cmd": "observe"})

    def step(self, frames=1, keys=(), mouse=None):
        command = {"cmd": "step", "frames": frames, "keys": list(keys)}
        if mouse is not None:
            command["mouse"] = mouse
        return self.request(command)

    def key(self, *keys):
        self.step()
        return self.step(keys=keys)

    def close(self):
        try:
            if sys.exc_info()[0] is None:
                successful(self.agent, {"cmd": "quit"})
                check(self.agent.process.wait(timeout=5) == 0, "clean runtime exit")
        finally:
            self.agent.close()

    def move_to(self, x, y, *, attack=False, limit=600):
        """Navigate only through held arrow input; no game-state mutation endpoint."""
        snapshot = self.observe()
        for _ in range(limit):
            p = actor(snapshot, "player")["position"]
            dx, dy = x - p["x"], y - p["y"]
            if abs(dx) <= 0.075 and abs(dy) <= 0.075:
                return self.step(keys=["space"] if attack else [])
            keys = ["space"] if attack else []
            if abs(dx) > 0.075:
                keys.append("right" if dx > 0 else "left")
            if abs(dy) > 0.075:
                keys.append("down" if dy > 0 else "up")
            distance = max(abs(dx), abs(dy))
            snapshot = self.step(6 if distance > 1 else 1, keys=keys)
            check(state(snapshot)["phase"] != "failed", "player failed while navigating")
        raise AssertionError(f"could not reach {(x,y)}: {actor(snapshot,'player')['position']}")


def decode_slots(encoded):
    return [(item, int(count), identity) for item, count, identity in
            (entry.split(",") for entry in encoded.split(";"))]


def amount(snapshot, item):
    s = state(snapshot)
    return sum(count for name, count, _ in decode_slots(s["bag_state"]) if name == item)


def gameplay(snapshot):
    """Compare actual world/native observations while excluding diagnostic counters."""
    result = copy.deepcopy(snapshot["actors"])
    for a in result:
        for c in a["components"]:
            values = c.get("properties", {})
            for key in ("save_status", "load_status", "last_error", "last_result", "save_count", "load_count"):
                values.pop(key, None)
    return result


def wait_for(r, predicate, message, frames=1200, keys=()):
    snapshot = r.observe()
    for _ in range(frames):
        if predicate(snapshot):
            return snapshot
        snapshot = r.step(keys=keys)
    raise AssertionError(message + ": " + str(state(snapshot)))


def portal(r, destination):
    if destination == "underworld":
        r.move_to(4.75, 2.5)
        direction = "right"
    else:
        r.move_to(19.25, 2.5)
        direction = "left"
    wait_for(r, lambda s: state(s)["transition_remaining"] > 0,
             "portal did not start", 30, [direction])
    return wait_for(r, lambda s: state(s)["world_id"] == destination and
                    state(s)["transition_remaining"] == 0,
                    "portal did not finish", 60)


def fight(r, name, max_ticks=240):
    snapshot = r.observe()
    for _ in range(max_ticks):
        enemy = properties(snapshot, name, "Combatant")
        if enemy["health"] <= 0:
            return r.step()
        p, q = actor(snapshot, "player")["position"], actor(snapshot, name)["position"]
        keys = ["space"]
        if abs(p["x"] - q["x"]) > 0.8:
            keys.append("right" if p["x"] < q["x"] else "left")
        if abs(p["y"] - q["y"]) > 0.8:
            keys.append("down" if p["y"] < q["y"] else "up")
        snapshot = r.step(keys=keys)
        check(state(snapshot)["phase"] == "playing", "player failed during real combat")
    raise AssertionError("could not defeat " + name)


def basic_replay(runtime, project, saves):
    r = Replay(runtime, project, saves)
    try:
        r.step()
        initial = r.observe()
        check(state(initial)["ready"] and amount(initial, "seed") == 8 and amount(initial, "food") == 0,
              "authored seed/food inventory incorrect")
        commands = [{"cmd": "step", "frames": 15, "keys": ["up"]},
                    {"cmd": "step", "frames": 12, "keys": ["right"]},
                    {"cmd": "step", "frames": 4, "keys": []}]
        recorded = [r.request(c) for c in commands]
        check(actor(recorded[-1], "player")["position"] != actor(initial, "player")["position"],
              "real movement did not change player position")
        r.request({"cmd": "reset"})
        r.step()
        check(r.observe() == initial, "reset did not reproduce complete initial state")
        for command, expected in zip(commands, recorded):
            check(r.request(command) == expected, "identical input replay diverged")
    finally:
        r.close()


def complete_loop(runtime, project, saves):
    r = Replay(runtime, project, saves)
    try:
        r.step()
        r.move_to(-4.5, -2.5)
        planted = r.key("e")
        check(amount(planted, "seed") == 7, "planting did not consume exactly one real seed")
        r.step(120)
        harvested = r.key("e")
        collected = r.step()
        check(amount(collected, "food") == 1, "mature harvest did not enter native inventory")
        check(amount(collected, "seed") == 7, "harvesting unexpectedly consumed or returned seed")
        equipped = r.key("1")
        check(state(equipped)["attack"] == 3, "sword did not change actual attack")
        r.move_to(4.0, -1.5, attack=True)
        wait_for(r, lambda s: state(s)["route_phase"] == "active", "first wave missing", 1000)
        for name in ("invader_1", "invader_2", "invader_3"):
            fight(r, name)
        defended = r.step()
        check(state(defended)["route_phase"] == "countdown" and not state(defended)["route_closed"],
              "clearing a wave incorrectly granted permanent safety")
        check(state(defended)["wave"] >= 1, "real wave was not recorded")
        remaining = state(defended)["countdown_remaining"]
        arrived = portal(r, "underworld")
        check(state(arrived)["countdown_remaining"] < remaining,
              "leaving village froze invasion countdown")
        # Real underground enemy damage establishes food's combat effect.
        r.move_to(22.0, 1.5)
        injured = wait_for(r, lambda s: state(s)["player_health"] < 6,
                          "guardian never damaged player", 180)
        check(state(injured)["player_health"] > 0, "guardian killed player before food check")
        fight(r, "guardian")
        before = r.observe()
        healed = r.key("q")
        check(amount(healed, "food") == amount(before, "food") - 1 and
              state(healed)["player_health"] == min(6, state(before)["player_health"] + 3),
              "food did not consume one item and apply catalog healing")
        fight(r, "source")
        closed = r.step()
        check(state(closed)["route_closed"] and state(closed)["route_phase"] == "closed" and
              state(closed)["source_health"] == 0 and state(closed)["active_invaders"] == 0,
              "source death did not permanently close its route")
        back = portal(r, "village")
        check(state(back)["world_id"] == "village", "return portal failed")
        idle = r.step(1000)
        check(state(idle)["route_phase"] == "closed" and state(idle)["active_invaders"] == 0,
              "closed route spawned another wave")
        saved = r.key("f5")
        check(state(saved)["save_status"] == "saved", "stable closed-route checkpoint failed")
        path = saves / "two_world_village/main.save.json"
        check(path.is_file(), "checkpoint missing from explicit save namespace")
        return json.loads(path.read_text()), saved
    finally:
        r.close()


def checkpoint_recovery(runtime, project, saves, checkpoint):
    path = saves / "two_world_village/main.save.json"
    r = Replay(runtime, project, saves)
    try:
        restored = r.step(keys=["f9"])
        check(state(restored)["load_status"] == "loaded" and state(restored)["route_closed"],
              "new process did not restore permanent closure")
        # Saving on the following distinct input tick must retain the exact payload.
        saved = r.step(keys=["f5"])
        check(state(saved)["save_status"] == "saved" and json.loads(path.read_text())["data"] == checkpoint["data"],
              "new process changed persisted world/inventory fields")
        bad = []
        def variant(change):
            value = copy.deepcopy(checkpoint); change(value); bad.append(value)
        variant(lambda v: v.update(schema_version=999))
        variant(lambda v: v["data"]["stats"].update(defeated=-1))
        variant(lambda v: v["data"]["crops"][-1].update(stage=3, remaining_ticks=0))
        variant(lambda v: v["data"]["route"].update(source_health=1))
        variant(lambda v: v["data"]["route"].update(remaining_ticks=1))
        variant(lambda v: v["data"]["player"].update(x=28))
        variant(lambda v: v["data"]["bag"][0].update(count=1.0))
        variant(lambda v: v["data"]["bag"][0].update(item_id="unknown"))
        variant(lambda v: v["data"]["equipment"][1].update(item_id="sword", count=1, instance_id="sword-island-1"))
        variant(lambda v: v["data"]["bag"][0].update(item_id="sword", count=1, instance_id="other-sword"))
        variant(lambda v: v["data"].update(unexpected=True))
        bad.append("{broken save")
        for index, value in enumerate(bad):
            r.step()
            before = r.observe()
            path.write_text(value if isinstance(value, str) else json.dumps(value))
            rejected = r.step(keys=["f9"])
            check(state(rejected)["load_status"] != "loaded", f"bad checkpoint {index} was accepted")
            check(gameplay(rejected) == gameplay(before), f"bad checkpoint {index} partially changed live state")
        path.write_text(json.dumps(checkpoint))
        r.request({"cmd": "reset"})
        fresh = r.step()
        check(not state(fresh)["route_closed"] and state(fresh)["source_health"] == 9,
              "reset unexpectedly auto-loaded disk checkpoint")
        check(json.loads(path.read_text()) == checkpoint, "reset changed disk checkpoint")
        loaded = r.step(keys=["f9"])
        check(state(loaded)["route_closed"], "explicit load after reset lost route closure")
    finally:
        r.close()


def full_bag_and_no_seed(runtime, project, saves, checkpoint):
    path = saves / "two_world_village/main.save.json"
    value = copy.deepcopy(checkpoint)
    d = value["data"]
    d["world_id"] = "village"; d["player"] = {"x": -4.5, "y": -2.5, "health": 6}
    d["bag"] = [{"item_id": "seed", "count": 99, "instance_id": ""} for _ in range(30)]
    d["bag"][0]["count"] = 1
    d["equipment"] = [{"item_id": "sword", "count": 1, "instance_id": "sword-island-1"}] + [
        {"item_id": "", "count": 0, "instance_id": ""} for _ in range(3)]
    d["crops"] = [{"stage": 2, "remaining_ticks": 0, "drop_count": 0} for _ in range(16)]
    d["crops"][0]["stage"] = 4
    d["stats"] = {"planted": 1, "harvested": 0, "consumed": 0, "crops_destroyed": 0, "defeated": 0}
    path.write_text(json.dumps(value))
    r = Replay(runtime, project, saves)
    try:
        loaded = r.step(keys=["f9"])
        check(state(loaded)["load_status"] == "loaded", "valid full-bag checkpoint rejected")
        seeds = amount(loaded, "seed")
        full = r.key("e")
        check(amount(full, "food") == 0 and amount(full, "seed") == seeds,
              "full bag harvest overwrote a slot or consumed a seed")
        held = r.step(12, ["e"])
        check(state(held)["harvested"] == 1, "held harvest input multiplied produce")
        saved = r.step(keys=["f5"])
        check(state(saved)["save_status"] == "saved", "could not save full-bag harvest remainder")
        retained = json.loads(path.read_text())["data"]
        check(retained["crops"][0]["drop_count"] == 1 and retained["crops"][0]["stage"] == 2,
              "full bag discarded harvested ground remainder")
        freed = r.key("e")
        collected = r.step()
        check(amount(collected, "seed") == seeds - 1 and amount(collected, "food") == 1,
              "freed native slot did not collect the exact retained ground item")
        r.step(keys=["f5"])
        check(json.loads(path.read_text())["data"]["crops"][0]["drop_count"] == 0,
              "collected ground item remained in world and could duplicate")
        # A second valid checkpoint supplies food but no seeds; planting must reject.
        d["bag"] = [{"item_id": "food", "count": 9, "instance_id": ""} for _ in range(30)]
        d["crops"][0]["stage"] = 2; d["stats"]["planted"] = 0
        path.write_text(json.dumps(value))
        r.step()
        loaded = r.step(keys=["f9"])
        check(state(loaded)["load_status"] == "loaded", "valid seedless checkpoint rejected")
        rejected = r.key("e")
        check(amount(rejected, "seed") == 0 and amount(rejected, "food") == 270 and state(rejected)["planted"] == 0,
              "seedless planting changed native items or created a crop")
    finally:
        r.close()


def active_closure_and_busy(runtime, project, saves):
    r = Replay(runtime, project, saves)
    path = saves / "two_world_village/main.save.json"
    try:
        r.step(); r.key("1")
        r.move_to(-4.5, -2.5); r.key("e")
        portal(r, "underworld")
        fight(r, "guardian")
        r.move_to(26.4, -.5)
        saved = r.key("f5")
        check(state(saved)["save_status"] == "saved", "underground stable checkpoint failed")
        contents = path.read_bytes()
        check(json.loads(contents)["data"]["crops"][0]["stage"] == 4,
              "crop growth stopped when player left village")
        wait_for(r, lambda s: state(s)["route_phase"] == "active", "underground wait froze village invasion", 1000)
        busy = r.key("f5")
        check(state(busy)["save_status"] == "busy" and path.read_bytes() == contents,
              "active-wave save overwrote a stable checkpoint")
        closed = r.step(36, ["space"])
        check(state(closed)["route_closed"] and state(closed)["active_invaders"] == 0,
              "source death did not cancel an active invasion")
        later = r.step(1000)
        check(state(later)["route_closed"] and state(later)["active_invaders"] == 0,
              "active cancellation did not remain permanent")
    finally:
        r.close()


def weapon_damage(runtime, project, saves, checkpoint):
    path = saves / "two_world_village/main.save.json"
    attack_counts = []
    for armed in (False, True):
        value = copy.deepcopy(checkpoint); d = value["data"]
        d["world_id"] = "underworld"; d["player"] = {"x": 23.3, "y": 1.5, "health": 6}
        d["guardian"] = {"x": 24.5, "y": 1.5, "health": 6}
        d["route"].update(closed=False, remaining_ticks=900, source_health=9)
        if not armed:
            d["bag"][2] = d["equipment"][0]
            d["equipment"][0] = {"item_id": "", "count": 0, "instance_id": ""}
        path.write_text(json.dumps(value))
        r = Replay(runtime, project, saves)
        try:
            loaded = r.step(keys=["f9"])
            check(state(loaded)["load_status"] == "loaded", "valid combat checkpoint rejected")
            before = state(loaded)["attacks"]
            hit = r.step(keys=["space"])
            check(properties(hit, "guardian", "Combatant")["health"] == (3 if armed else 5),
                  "native weapon stats did not change damage to actual enemy")
            dead = wait_for(r, lambda x: properties(x, "guardian", "Combatant")["health"] == 0,
                            "enemy survived expected bounded attacks", 80, ["space"])
            attack_counts.append(state(dead)["attacks"] - before)
            defeated = state(dead)["defeated"]
            again = r.step(36, ["space"])
            check(state(again)["defeated"] == defeated, "dead enemy settled its defeat twice")
        finally:
            r.close()
    check(attack_counts == [6, 2], "weapon did not change actual kill count: " + str(attack_counts))


def crop_priority_and_failure_recovery(runtime, project, saves):
    r = Replay(runtime, project, saves)
    try:
        r.step(); saved = r.step(keys=["f5"])
        check(state(saved)["save_status"] == "saved", "initial recovery checkpoint failed")
        r.move_to(-4.5, -2.5); r.key("e"); portal(r, "underworld")
        wait_for(r, lambda x: state(x)["route_phase"] == "active", "wave failed to activate", 1000)
        active = r.step()
        check(state(active)["altar_health"] == 8 and all(
            properties(active, name, "Combatant")["target_kind"] == "crop"
            for name in ("invader_1", "invader_2", "invader_3")), "invaders did not prioritize living crop")
        lost_crop = wait_for(r, lambda x: state(x)["crops_destroyed"] == 1,
                             "invaders never reached and destroyed crop", 800)
        check(state(lost_crop)["altar_health"] == 8 and amount(lost_crop, "food") == 0,
              "crop attack also damaged altar or awarded a harvest")
        hurt = wait_for(r, lambda x: state(x)["altar_health"] < 8,
                        "invaders did not retarget altar", 800)
        check(state(hurt)["world_id"] == "underworld", "test accidentally returned to village")
        failed = wait_for(r, lambda x: state(x)["phase"] == "failed", "undefended altar never failed", 240)
        check(state(failed)["altar_health"] == 0, "failure was not actual altar destruction")
        recovered = r.step(keys=["f9"])
        check(state(recovered)["load_status"] == "loaded" and state(recovered)["phase"] == "playing" and
              state(recovered)["altar_health"] == 8 and state(recovered)["active_invaders"] == 0,
              "failed village could not load a stable checkpoint")
    finally:
        r.close()


def missing_partner(runtime, project, saves, checkpoint):
    with tempfile.TemporaryDirectory(prefix="pocket-missing-portal-") as tmp:
        fixture = Path(tmp) / "project"; shutil.copytree(project, fixture)
        (fixture / "component_types/RemovePartner.lua").write_text(
            "RemovePartner={OnStart=function(self) Actor.Destroy(Actor.Find('underworld_portal')) end}\n")
        scene_path = fixture / "scenes/village.scene"
        scene = json.loads(scene_path.read_text())
        scene["actors"].append({"uid": 900, "name": "remove_partner", "components": {"test": {"type": "RemovePartner"}}})
        scene_path.write_text(json.dumps(scene))
        r = Replay(runtime, fixture, saves)
        try:
            r.step(); r.move_to(4.75, 2.5)
            rejected = r.step(10, ["right"])
            check(state(rejected)["last_result"] == "missing_portal" and state(rejected)["transition_remaining"] == 0 and
                  state(rejected)["world_id"] == "village", "missing endpoint trapped player in transition")
            x = actor(rejected, "player")["position"]["x"]
            moved = r.step(8, ["left"])
            check(actor(moved, "player")["position"]["x"] < x - .3, "portal rejection did not restore controls")
            r.step(); before = r.observe()
            (saves / "two_world_village/main.save.json").write_text(json.dumps(checkpoint))
            refused = r.step(keys=["f9"])
            check(state(refused)["load_status"] == "missing_portal" and gameplay(before) == gameplay(refused),
                  "missing fixed world reference allowed partial checkpoint restoration")
        finally:
            r.close()


def failure_during_transition(runtime, project, saves, checkpoint):
    with tempfile.TemporaryDirectory(prefix="pocket-transition-failure-") as tmp:
        fixture = Path(tmp) / "project"; shutil.copytree(project, fixture)
        scene_path = fixture / "scenes/village.scene"; scene = json.loads(scene_path.read_text())
        for a in scene["actors"]:
            if a["name"].startswith("invader_"):
                a["components"]["transform"].update(x=2.5, y=.5)
                a["components"]["body"].update(x=2.5, y=.5)
        scene_path.write_text(json.dumps(scene))
        value = copy.deepcopy(checkpoint); d = value["data"]
        d["world_id"] = "village"; d["player"] = {"x": 4.75, "y": 2.5, "health": 6}
        d["route"].update(closed=False, source_health=9, remaining_ticks=15, altar_health=2)
        (saves / "two_world_village/main.save.json").write_text(json.dumps(value))
        r = Replay(runtime, fixture, saves)
        try:
            loaded = r.step(keys=["f9"])
            check(state(loaded)["load_status"] == "loaded", "valid transition failure setup rejected")
            r.step(9)  # Re-arm outside the endpoint while the load grace elapses.
            transitioning = r.step(4, ["right"])
            check(state(transitioning)["transition_remaining"] > 0, "setup did not enter real portal")
            failed = r.step(12)
            check(state(failed)["phase"] == "failed" and state(failed)["transition_remaining"] == 0 and
                  state(failed)["transition_alpha"] == 0, "failure left portal fade permanently busy")
            recovered = r.step(keys=["f9"])
            check(state(recovered)["load_status"] == "loaded" and state(recovered)["phase"] == "playing",
                  "failure during portal transition blocked checkpoint recovery")
        finally:
            r.close()


def mouse_keyboard_parity(runtime, project, saves, checkpoint):
    planted = []
    for mouse in (False, True):
        r = Replay(runtime, project, saves)
        try:
            r.step(); r.move_to(-4.5, -2.5); r.step()
            if mouse:
                snapshot = r.step(14, mouse={"x": 300, "y": 300, "buttons": [3]})
            else:
                snapshot = r.step(14, ["e"])
            check(amount(snapshot, "seed") == 7, "farm input did not consume one seed")
            planted.append(gameplay(snapshot))
        finally:
            r.close()
    check(planted[0] == planted[1], "keyboard and mouse farming diverged")
    value = copy.deepcopy(checkpoint); d = value["data"]
    d["world_id"] = "underworld"; d["player"] = {"x": 23.3, "y": 1.5, "health": 6}
    d["guardian"] = {"x": 24.5, "y": 1.5, "health": 6}
    d["route"].update(closed=False, source_health=9, remaining_ticks=900)
    (saves / "two_world_village/main.save.json").write_text(json.dumps(value))
    attacks = []
    for mouse in (False, True):
        r = Replay(runtime, project, saves)
        try:
            r.step(keys=["f9"])
            if mouse:
                snapshot = r.step(14, mouse={"x": 400, "y": 300, "buttons": [1]})
            else:
                snapshot = r.step(14, ["space"])
            check(properties(snapshot, "guardian", "Combatant")["health"] == 0, "held combat input did not defeat guardian")
            attacks.append(gameplay(snapshot))
        finally:
            r.close()
    check(attacks[0] == attacks[1], "keyboard and mouse combat diverged")


def portal_reentry(runtime, project, saves, checkpoint):
    (saves / "two_world_village/main.save.json").write_text(json.dumps(checkpoint))
    r = Replay(runtime, project, saves)
    try:
        r.step(keys=["f9"])
        for _ in range(2):
            r.move_to(4.75, 2.5)
            wait_for(r, lambda x: state(x)["transition_remaining"] > 0,
                     "held-input portal did not begin", 20, ["right"])
            held = r.step(55, ["right"])
            check(state(held)["world_id"] == "underworld" and state(held)["transition_remaining"] == 0,
                  "held input produced portal ping-pong")
            p = actor(held, "player")["position"]
            idle = r.step(60)
            check(state(idle)["world_id"] == "underworld" and actor(idle, "player")["position"] == p,
                  "stationary portal arrival bounced or drifted")
            returned = portal(r, "village")
            check(state(returned)["world_id"] == "village", "paired return failed after held input")
    finally:
        r.close()


def actual_inventory_rejection(runtime, project, saves, checkpoint):
    with tempfile.TemporaryDirectory(prefix="pocket-native-restore-rejection-") as tmp:
        fixture = Path(tmp) / "project"; shutil.copytree(project, fixture)
        (fixture / "component_types/ForeignCatalog.lua").write_text("""
ForeignCatalog={OnStart=function(self)
 local core=Actor.Find('village'):GetComponent('VillageState')
 local other=Inventory.Open('village.items.json')
 core.equip=other:Create(4,{'subweapon','head','chest','legs'})
 core:Publish()
end}
""")
        scene_path = fixture / "scenes/village.scene"; scene = json.loads(scene_path.read_text())
        scene["actors"].append({"uid": 900, "name": "foreign_catalog", "components": {"test": {"type": "ForeignCatalog"}}})
        scene_path.write_text(json.dumps(scene))
        (saves / "two_world_village/main.save.json").write_text(json.dumps(checkpoint))
        r = Replay(runtime, fixture, saves)
        try:
            r.step(); before = r.observe()
            refused = r.step(keys=["f9"])
            check(state(refused)["load_status"] == "invalid_inventory" and gameplay(refused) == gameplay(before),
                  "actual incompatible-catalog RestoreGroup partially changed inventory/world")
        finally:
            r.close()


def run(runtime, project, saves):
    basic_replay(runtime, project, saves)
    checkpoint, _ = complete_loop(runtime, project, saves)
    checkpoint_recovery(runtime, project, saves, checkpoint)
    full_bag_and_no_seed(runtime, project, saves, checkpoint)
    active_closure_and_busy(runtime, project, saves)
    weapon_damage(runtime, project, saves, checkpoint)
    crop_priority_and_failure_recovery(runtime, project, saves)
    missing_partner(runtime, project, saves, checkpoint)
    failure_during_transition(runtime, project, saves, checkpoint)
    mouse_keyboard_parity(runtime, project, saves, checkpoint)
    portal_reentry(runtime, project, saves, checkpoint)
    actual_inventory_rejection(runtime, project, saves, checkpoint)


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_two_world_village_test.py RUNTIME FIXTURE")
    with tempfile.TemporaryDirectory(prefix="pocket-two-world-") as temporary:
        run(str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve(), Path(temporary))
    print("agent_two_world_village_test: replay, farm/inventory/combat/portals, persistence and rejection paths passed")
