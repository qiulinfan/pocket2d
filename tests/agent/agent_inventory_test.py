#!/usr/bin/env python3
"""Real mouse/keyboard inventory workflow through the supervisor and Lua facade."""
import copy
import json
import shutil
from pathlib import Path
import sys
import tempfile
from agent_protocol_test import actor, check, properties, successful
from agent_savegame_test import SaveAgent


def state(snapshot):
    return properties(snapshot, "inventory", "InventoryState")


def slots(snapshot, container):
    value = state(snapshot)[container + "_state"]
    if not value:
        return []
    return [(item, int(count), identity) for item, count, identity in
            (slot.split(",") for slot in value.split(";"))]


def position(snapshot):
    return actor(snapshot, "player")["position"]


def integrity(snapshot):
    s = state(snapshot)
    if not s["ready"]:
        return
    totals = {"herb": s["world_remaining"] + s["consumed"], "ore": 0,
              "boots": 0, "sword": 0, "helmet": 0, "armor": 0}
    identities = set()
    for container, count in [("bag", 30), ("chest", 12), ("equipment", 4)]:
        values = slots(snapshot, container)
        check(len(values) == count, "wrong native container capacity")
        for index, (item, amount, identity) in enumerate(values):
            if not item:
                check(amount == 0 and identity == "", "noncanonical empty slot")
                continue
            check(item in totals and 0 < amount <= (5 if item in ["herb", "ore"] else 1),
                  "unknown item or invalid stack count")
            totals[item] += amount
            if item in ["herb", "ore"]:
                check(identity == "", "stack acquired an instance identity")
            else:
                check(identity == item + "-1" and identity not in identities,
                      "gear identity duplicated, changed or lost")
                identities.add(identity)
            if container == "equipment":
                check(item == ["sword", "helmet", "armor", "boots"][index], "equipment filter violated")
    check(totals == {"herb": 16, "ore": 139, "boots": 1, "sword": 1, "helmet": 1, "armor": 1},
          "item conservation failed: " + str(totals))
    check(s["total_herb"] == 16 and s["total_ore"] == 139 and s["total_gear"] == 4,
          "published conservation differs from independently decoded slots")
    mapping = [int(value) for value in s["hotbar_state"].split(",")]
    check(len(mapping) == len(set(mapping)) == 9 and all(1 <= i <= 30 for i in mapping),
          "hotbar is not nine distinct bag references")


def contents(snapshot):
    s = state(snapshot)
    return tuple(s[k] for k in ["bag_state", "chest_state", "equipment_state",
                               "bag_revision", "chest_revision", "equipment_revision",
                               "world_remaining", "consumed"])


def cell(container, index):
    if container == "bag":
        x, y, columns = 40, 210, 6
    elif container == "chest":
        x, y, columns = 500, 210, 4
    elif container == "equipment":
        x, y, columns = 500, 430, 4
    else:
        x, y, columns = 40, 550, 9
    return x + (index - 1) % columns * 58 + 26, y + (index - 1) // columns * 58 + 26


class Replay:
    def __init__(self, runtime, project, root):
        self.runtime, self.project, self.root = runtime, project, root
        self.agent = SaveAgent(runtime, project, root)
        self.commands = 0

    def request(self, command):
        observed = successful(self.agent, command)
        integrity(observed)
        self.commands += 1
        return observed

    def observe(self):
        return self.request({"cmd": "observe"})

    def step(self, frames=1, keys=(), mouse=None):
        command = {"cmd": "step", "frames": frames, "keys": list(keys)}
        if mouse is not None:
            command["mouse"] = mouse
        return self.request(command)

    def drag(self, source, source_index, target, target_index, button=1):
        x, y = cell(source, source_index)
        before = self.observe()
        held = self.step(mouse={"x": x, "y": y, "buttons": [button]})
        check(state(held)["dragging"] and contents(held) == contents(before),
              "begin drag removed or changed source ownership")
        x, y = cell(target, target_index)
        moved = self.step(3, mouse={"x": x, "y": y, "buttons": [button]})
        check(contents(moved) == contents(before), "held drag committed before mouse up")
        return self.step(mouse={"x": x, "y": y, "buttons": []})

    def click(self, container, index):
        x, y = cell(container, index)
        self.step(mouse={"x": x, "y": y, "buttons": [1]})
        return self.step(mouse={"x": x, "y": y, "buttons": []})

    def key(self, *keys):
        self.step()
        return self.step(keys=keys)

    def close(self):
        self.agent.close()


def run(runtime, project, root):
    path = root / "inventory_village/main.save.json"
    r = Replay(runtime, project, root)
    try:
        initial = r.observe()
        first = r.step()
        check(slots(first, "bag")[0] == ("herb", 4, "") and slots(first, "bag")[3] == ("", 0, ""),
              "authored bag state incorrect")
        picked = r.step(18, ["d"])
        check(state(picked)["world_remaining"] == 2 and state(picked)["pickup_moved"] == 6 and
              slots(picked, "bag")[0] == ("herb", 5, "") and slots(picked, "bag")[3] == ("herb", 5, ""),
              "real trigger pickup did not fill partial stacks and retain remainder")
        before = contents(picked)
        full = r.key("e")
        check(contents(full) == before, "full bag retry lost or duplicated world items")
        saved = r.key("f5")
        check(state(saved)["save_status"] == "saved" and path.is_file(), "initial partial-pickup save failed")
        partial_bytes = path.read_bytes()
        check(json.loads(partial_bytes)["data"]["world_remaining"] == 2, "wrong remaining pile persisted")
    finally:
        r.close()

    r = Replay(runtime, project, root)
    try:
        loaded = r.step(keys=["f9"])
        check(state(loaded)["load_status"] == "loaded" and state(loaded)["world_remaining"] == 2,
              "fresh process regenerated the full pickup instead of its remainder")
        check(properties(loaded, "herb_pickup", "InventoryPickup")["remaining"] == 2,
              "world pickup actor does not reflect restored native inventory remainder")
        # Screen-exterior, same-slot and Escape cancellation preserve source and revisions.
        r.step()
        before = contents(r.observe())
        x, y = cell("bag", 1)
        r.step(mouse={"x": x, "y": y, "buttons": [1]})
        r.step(5, mouse={"x": -10, "y": -10, "buttons": [1]})
        cancelled = r.step(mouse={"buttons": []})
        check(contents(cancelled) == before and not state(cancelled)["dragging"], "outside cancel mutated ownership")
        same = r.click("bag", 1)
        check(contents(same) == before, "same-slot click changed inventory revision")
        r.step(mouse={"x": x, "y": y, "buttons": [1]})
        r.step(keys=["escape"], mouse={"x": cell("chest", 6)[0], "y": cell("chest", 6)[1], "buttons": [1]})
        cancelled = r.step(mouse={"buttons": []})
        check(contents(cancelled) == before, "Escape followed by release committed a cancelled drag")
        split = r.drag("bag", 1, "chest", 6, button=3)
        check(slots(split, "bag")[0] == ("herb", 2, "") and slots(split, "chest")[5] == ("herb", 3, ""),
              "right-button split did not transfer ceil(5/2) into empty destination")
        before = contents(split)
        rejected = r.drag("chest", 6, "bag", 2, button=3)
        check(contents(rejected) == before, "split into occupied incompatible slot partially changed state")
        merged = r.drag("bag", 1, "chest", 4)
        check(slots(merged, "bag")[0] == ("herb", 1, "") and slots(merged, "chest")[3] == ("herb", 5, ""),
              "partial merge lost source remainder or exceeded stack limit")
        merged = r.drag("chest", 6, "bag", 1)
        check(slots(merged, "bag")[0] == ("herb", 4, "") and slots(merged, "chest")[5][1] == 0,
              "reverse chest-to-bag merge failed")
        swapped = r.drag("bag", 3, "chest", 5)
        check(slots(swapped, "bag")[2] == ("ore", 4, "") and slots(swapped, "chest")[4] == ("boots", 1, "boots-1"),
              "cross-container swap lost counts or gear identity")
        r.drag("bag", 3, "chest", 5)
        before = contents(r.observe())
        invalid = r.drag("bag", 3, "equipment", 1)
        check(contents(invalid) == before, "legs item was accepted in subweapon slot")
        equipped = r.drag("bag", 3, "equipment", 4)
        check(state(equipped)["speed"] == 3 and slots(equipped, "equipment")[3] == ("boots", 1, "boots-1"),
              "boots did not preserve identity or change actual movement stat")
        before = contents(equipped)
        invalid = r.drag("equipment", 4, "chest", 1)
        check(contents(invalid) == before, "reverse swap placed sword into legs slot or lost boots")
        for source, destination, item in [(1, 1, "sword"), (2, 2, "helmet"), (3, 3, "armor")]:
            equipped = r.drag("chest", source, "equipment", destination)
            check(slots(equipped, "equipment")[destination - 1] == (item, 1, item + "-1"), "equipment identity changed")
        check(state(equipped)["attack"] == 3 and state(equipped)["defense"] == 4, "equipped stats not derived from catalog")
        start = position(equipped)["y"]
        fast = r.step(60, ["w"])
        check(abs(position(fast)["y"] - start + 3) < 0.002, "boots did not produce three real world units per 60 ticks")
        returned = r.step(60, ["s"])
        check(state(returned)["world_remaining"] == 0 and not any(a["name"] == "herb_pickup" for a in returned["actors"]),
              "re-entering pickup did not collect only its remaining two and destroy depleted actor")
        check(slots(returned, "bag")[2] == ("herb", 1, ""), "remainder did not occupy the newly empty slot")
        r.click("equipment", 4)
        before = contents(r.observe())
        full = r.key("q")
        check(contents(full) == before and state(full)["speed"] == 3, "full-bag unequip dropped or duplicated boots")
        r.drag("bag", 3, "chest", 6)
        r.click("equipment", 4)
        bare = r.key("q")
        check(state(bare)["speed"] == 2 and slots(bare, "bag")[2] == ("boots", 1, "boots-1"), "unequip failed after freeing space")
        start = position(bare)["y"]
        slow = r.step(60, ["s"])
        check(abs(position(slow)["y"] - start - 2) < 0.002, "unequipped movement did not return to two units per 60 ticks")
        r.step(60, ["w"])
        hit = r.key("space")
        check(state(hit)["target_health"] == 17, "equipped sword did not damage the actual training target")
        damaged = r.key("h")
        check(state(damaged)["health"] == 1, "armor did not reduce training damage from two to one")
        r.key("1")
        healed = r.key("u")
        check(state(healed)["health"] == 2 and state(healed)["consumed"] == 1, "food did not consume exactly one item and heal")
        held = r.step(6, ["u"])
        check(state(held)["consumed"] == 1, "held use key repeatedly consumed food")
        healed = r.key("u")
        check(state(healed)["health"] == 3 and state(healed)["consumed"] == 2, "second use edge failed")
        before = contents(healed)
        check(contents(r.key("u")) == before, "full-health food use consumed an item")
        for index in [1, 2, 3]:
            r.drag("equipment", index, "chest", index)
        check(state(r.key("h"))["health"] == 1, "unequipped defense did not restore two-point damage")
        check(state(r.key("space"))["target_health"] == 16, "unequipped attack did not restore one-point damage")
        r.drag("bag", 3, "equipment", 4)
        for index in [1, 2, 3]:
            r.drag("chest", index, "equipment", index)
        # Keyboard pick/drop uses the same native transactions.
        r.click("bag", 2)
        r.key("return")
        r.key("tab");r.key("right")
        moved = r.key("return")
        check(slots(moved, "bag")[1][1] == 0 and slots(moved, "chest")[1] == ("ore", 5, ""), "keyboard transfer failed")
        preview = r.key("left shift", "return")
        check(state(preview)["drag_count"] == 3, "keyboard split preview mismatch")
        r.key("escape")
        r.key("return");r.key("tab");r.key("tab");r.key("right")
        returned = r.key("return")
        check(slots(returned, "bag")[1] == ("ore", 5, "") and slots(returned, "chest")[1][1] == 0, "keyboard return transfer failed")
        before = contents(returned)
        mapped = r.drag("bag", 5, "hotbar", 2)
        check(contents(mapped) == before and state(mapped)["hotbar_state"] == "1,5,3,4,2,6,7,8,9", "hotbar remap moved or duplicated owned items")
        check(state(r.click("hotbar", 4))["selected"] == 4, "hotbar click did not select")
        r.key("9")
        cycled = r.step(5, mouse={"wheel": 1, "buttons": []})
        check(state(cycled)["selected"] == 1, "wheel impulse repeated across held multi-frame request")
        # Saving during an uncommitted preview cancels it without losing items.
        x, y = cell("bag", 1)
        before = contents(cycled)
        r.step(mouse={"x": x, "y": y, "buttons": [1]})
        saved = r.step(keys=["f5"], mouse={"buttons": [1]})
        check(state(saved)["save_status"] == "saved" and not state(saved)["dragging"] and contents(saved) == before,
              "save during drag lost preview ownership")
        r.step(mouse={"buttons": []})
        valid_bytes = path.read_bytes()
        expected = json.loads(valid_bytes)["data"]
        invalids = [(b'{"format_version":1,', "invalid_data")]
        envelope = json.loads(valid_bytes);envelope["schema_version"] = 99
        invalids.append((json.dumps(envelope).encode(), "unsupported_game_schema"))
        for mutate in [
            lambda d: d["bag"][-1].update(count=6),
            lambda d: d["equipment"][-1].update(instance_id="helmet-1"),
            lambda d: d["equipment"][-1].update(count=2),
            lambda d: d["player"].update(y="invalid"),
            lambda d: d["bag"].__setitem__(2, copy.deepcopy(d["equipment"][-1])),
            lambda d: d["hotbar_map"].__setitem__(8, d["hotbar_map"][0]),
            lambda d: d.update(world_remaining=8),
            lambda d: d["player"].update(x=2000),
            lambda d: d.update(consumed=-1),
        ]:
            envelope = json.loads(valid_bytes);envelope["data"]["health"] = 3;mutate(envelope["data"])
            invalids.append((json.dumps(envelope).encode(), "invalid_game_schema"))
        for payload, code in invalids:
            r.step()
            baseline = r.observe()
            path.write_bytes(payload)
            rejected = r.step(keys=["f9"])
            check(state(rejected)["load_status"] == code and contents(rejected) == contents(baseline)
                  and state(rejected)["health"] == state(baseline)["health"] and position(rejected) == position(baseline),
                  "late-invalid save partially restored containers, health or pose")
            check(path.read_bytes() == payload, "failed load overwrote the slot")
        path.write_bytes(valid_bytes)
        reset = r.request({"cmd": "reset"})
        check(reset == initial and path.read_bytes() == valid_bytes, "reset changed disk or authored initial snapshot")
        loaded = r.step(keys=["f9"])
        check(state(loaded)["load_status"] == "loaded" and state(loaded)["world_remaining"] == 0
              and state(loaded)["speed"] == 3 and state(loaded)["hotbar_state"] == "1,5,3,4,2,6,7,8,9", "reset worker failed to restore equipment/map/progression")
    finally:
        r.close()

    r = Replay(runtime, project, root)
    try:
        loaded = r.step(keys=["f9"])
        check(state(loaded)["load_status"] == "loaded" and state(loaded)["consumed"] == expected["consumed"]
              and not any(a["name"] == "herb_pickup" for a in loaded["actors"]), "fresh process regenerated depleted world items")
        for container in ["bag", "chest", "equipment"]:
            check(slots(loaded, container) == [(s["item_id"], s["count"], s["instance_id"]) for s in expected[container]], "fresh process lost exact slots/instance IDs")
        start = position(loaded)["y"]
        moved = r.step(60, ["s"])
        check(abs(position(moved)["y"] - start - 3) < 0.002, "restored boots no longer affected real movement")
        r.key("1")
        healed = r.key("u")
        check(state(healed)["health"] == 2 and state(healed)["consumed"] == expected["consumed"] + 1,
              "restored food was not actually consumable")
    finally:
        r.close()


def world_prepare_failure(runtime, fixture, root):
    for case in ["missing", "bad_candidate", "restore_rejected"]:
        project = root / case / "project"
        shutil.copytree(fixture, project, ignore=shutil.ignore_patterns(".pocketengine", "__pycache__"))
        template = project / "actor_templates/HerbPickup.template"
        original = template.read_bytes()
        # Startup uses identical inline components, leaving the reconstruction
        # template uncached so the edited fixture really exercises candidate QA.
        scene_path = project / "scenes/inventory.scene"
        scene = json.loads(scene_path.read_text())
        for item in scene["actors"]:
            if item["name"] == "herb_pickup":
                item.pop("template", None)
                item["components"] = json.loads(original)["components"]
        scene_path.write_text(json.dumps(scene))
        r = Replay(runtime, project, root / case / "saves")
        try:
            r.step()
            check(state(r.key("f5"))["save_status"] == "saved", "could not prepare world-rebuild checkpoint")
            r.key("3", "u")
            drained = r.step(25, ["d"])
            check(state(drained)["world_remaining"] == 0, "probe did not deplete the real world pickup")
            expected = "world_prepare_failed"
            if case == "missing":
                template.unlink()
            elif case == "bad_candidate":
                template.write_text('{"name":"herb_pickup","components":{"transform":{"type":"Transform"}}}')
            else:
                path = root / case / "saves/inventory_village/main.save.json"
                saved = json.loads(path.read_text())
                # Exact numeric value passes the game's range/schema validation,
                # but native raw slots require a Lua integer representation.
                saved["data"]["bag"][0]["count"] = 4.0
                path.write_text(json.dumps(saved))
                expected = "invalid_inventory"
            # Last frame genuinely moves; failed F9 must stop stale velocity.
            baseline = r.step(keys=["d"])
            failed = r.step(keys=["f9"])
            check(state(failed)["load_status"] == expected,
                  "unexpected world preparation result: " + state(failed)["load_status"])
            check(contents(failed) == contents(baseline) and position(failed) == position(baseline)
                  and state(failed)["health"] == state(baseline)["health"],
                  "world preparation failure partially restored inventory, scalars or pose")
            check([(a["uid"], a["name"]) for a in failed["actors"]] ==
                  [(a["uid"], a["name"]) for a in baseline["actors"]], "failed world candidate leaked an actor")
            following = r.step()
            check(contents(following) == contents(baseline) and len(following["actors"]) == len(baseline["actors"]),
                  "published cache hid a partial restore or deferred candidate survived")
            if case == "missing":
                template.write_bytes(original)
                restored = r.key("f9")
                check(state(restored)["load_status"] == "loaded" and state(restored)["world_remaining"] == 8
                      and properties(restored, "herb_pickup", "InventoryPickup")["remaining"] == 8,
                      "valid world reconstruction did not recover after preparation failure")
        finally:
            r.close()


def stale_preview(runtime, fixture, root):
    project = root / "project"
    shutil.copytree(fixture, project, ignore=shutil.ignore_patterns(".pocketengine", "__pycache__"))
    path = project / "scenes/inventory.scene"
    scene = json.loads(path.read_text())
    scene["actors"].append({"uid": 8, "name": "revision_probe", "components": {
        "probe": {"type": "InventoryRevisionProbe"}}})
    path.write_text(json.dumps(scene))
    r = Replay(runtime, project, root / "saves")
    try:
        before = r.step()
        x, y = cell("bag", 1)
        changed = r.step(mouse={"x": x, "y": y, "buttons": [1]})
        check(state(changed)["dragging"] and state(changed)["consumed"] == 1
              and state(changed)["bag_revision"] > state(before)["bag_revision"],
              "cooperating gameplay callback did not actually mutate the native source")
        check(slots(changed, "bag")[0] == ("herb", 3, ""), "probe did not consume the real herb")
        x, y = cell("chest", 6)
        rejected = r.step(mouse={"x": x, "y": y, "buttons": []})
        check(state(rejected)["last_result"] == "stale_source" and not state(rejected)["dragging"]
              and contents(rejected) == contents(changed), "stale drag revision moved or duplicated mutated source items")
    finally:
        r.close()


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_inventory_test.py RUNTIME FIXTURE_PROJECT")
    runtime, fixture = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="pocket-inventory-game-") as directory:
        run(runtime, fixture, Path(directory) / "gameplay")
        world_prepare_failure(runtime, fixture, Path(directory) / "world-prepare")
        stale_preview(runtime, fixture, Path(directory) / "stale-preview")
    print("agent_inventory_test: mouse/keyboard transactions, conservation, gear identity/stats, real pickups and atomic persistence passed")
