#!/usr/bin/env python3
"""The scripted-player Session drives fixtures/agent_session through real
input (actions, keys, mouse at screen and world points), game observations,
PNG captures, a fresh save directory and reset; and reads tilemap cells of
fixtures/tilemap_farm."""
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "clients/python"))
from pocket2d import Session  # noqa: E402

FIXTURES = Path(__file__).resolve().parent / "fixtures"


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-session-") as directory:
        out = Path(directory) / "shots"
        with Session(FIXTURES / "agent_session", seed=5, out=out, select={"names": ["probe", "marker"]},
                     executable=executable) as session:
            saves = session.save_dir
            check(saves.is_dir() and not any(saves.iterdir()), "a fresh, empty save directory")
            check((session.screen_width, session.screen_height, session.zoom) == (320, 180, 0.5), "rendering.config is read")

            session.step(frames=2)
            probe = lambda: session.value("probe")  # noqa: E731
            check(probe()["jumps"] == 0 and session.values.keys() == {"probe"}, "game observations by key")
            session.press("jump")
            check(probe()["jumps"] == 1, "press() is one action edge: %r" % probe())
            session.step(keys=["d"], frames=3)
            session.step()
            check(probe()["held_right"] == 3, "keys are held for the step and released by default")
            session.key("k", frames=4)
            check(probe()["key_presses"] == 1, "key() holds for frames with one down edge")
            session.step({"move": [1, 0]}, frames=2)
            session.step()
            check(probe()["moved_x"] == 2, "actions are held for the step and released by default")

            check(session.world_to_screen(3, 2, (2, 1)) == (210, 140), "100 px per unit at zoom 1, centred")
            session.click_world(3, 2, camera=(2, 1))
            session.step()
            p = probe()
            check(p["clicks"] == 1 and abs(p["click_x"] - 3) < 1e-4 and abs(p["click_y"] - 2) < 1e-4,
                  "a world-point click lands at that point: %r" % p)
            session.step(mouse={"buttons": [1]})
            session.step(mouse={"buttons": []})
            p = probe()
            check(p["clicks"] == 2 and abs(p["click_x"] - 3) < 1e-4, "the mouse keeps its position: %r" % p)
            marker = session.actor("marker")["position"]
            check((marker["x"], marker["y"]) == (3, 2), "actors of the last step")

            session.key("f5")
            check(probe()["saved"], "the game saves")
            check(list((saves / "session_fixture").iterdir()), "saves land in the session's directory")

            path = session.shot("frame")
            check(path == out / "frame.png" and session.save_image(session.response, "again.png") == out / "again.png",
                  "images are named under out")
            data = path.read_bytes()
            check(data[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", data[16:24]) == (320, 180),
                  "the capture is a PNG at the configured resolution")
            frames = session.frames
            check(frames == 2 + 2 + 4 + 5 + 3 + 3 + 2 + 2 + 1, "frames counts every stepped frame: %d" % frames)

            session.reset()
            session.step()
            check(probe()["jumps"] == 0 and probe()["clicks"] == 0, "reset restarts the game")
            check(not session.errors, "no failing observers: %r" % session.errors)
        check(not saves.exists(), "the temporary save directory is removed on close")

        own = Path(directory) / "own-saves"
        own.mkdir()
        world = json.loads((FIXTURES / "tilemap_farm/maps/farm.tilemap.json").read_text())
        ground = world["layers"]["ground"]["cells"]
        with Session(FIXTURES / "tilemap_farm", game=False, save_dir=own, executable=executable) as farm:
            check(farm.tiles(10, "map", "ground", [[0, 0], [5, 2]]) == [ground[0], ground[2 * world["width"] + 5]],
                  "tiles() reads layer cells")
        check(own.is_dir(), "a given save directory is kept")
    print("agent session passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
