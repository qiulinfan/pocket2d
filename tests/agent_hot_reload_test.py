#!/usr/bin/env python3
"""reload_assets: images changed on disk show up in the next rendered frame."""
import base64
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import zlib
from agent_image_test import decode_png
from agent_protocol_test import Agent, check, successful


def write_solid_png(path, rgba, width=8, height=8):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgba) * width for _ in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def center_pixel(response):
    width, height, pixels = decode_png(base64.b64decode(response["image"]["data"]))
    return pixels[(height // 2) * width + width // 2]


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-hot-reload-") as directory:
        root = Path(directory)
        for folder in ("scenes", "images"):
            (root / folder).mkdir()
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "rendering.config").write_text('{"x_resolution":64,"y_resolution":64,"zoom_factor":4}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [{"uid": 1, "name": "box", "components": {
            "transform": {"type": "Transform"}, "sprite": {"type": "SpriteRenderer", "sprite": "box.png"}}}]}))
        write_solid_png(root / "images/box.png", (200, 30, 30, 255))
        agent = Agent(executable, str(root))
        try:
            image = {"width": 64, "height": 64}
            before = center_pixel(successful(agent, {"cmd": "step", "image": image}))
            check(before[:3] == bytes((200, 30, 30)), "initial sprite color: %r" % before)
            unchanged = successful(agent, {"cmd": "reload_assets"})
            check(unchanged["reload_assets"] == {"images": 0, "spritesheet_metadata": False}, "nothing changed yet")
            write_solid_png(root / "images/box.png", (30, 30, 200, 255))
            stamp = time.time() + 2
            os.utime(root / "images/box.png", (stamp, stamp))  # coarse filesystem clocks
            check(center_pixel(successful(agent, {"cmd": "observe", "image": image}))[:3] == bytes((200, 30, 30)),
                  "the cached texture stays until reloaded")
            reloaded = successful(agent, {"cmd": "reload_assets"})
            check(reloaded["reload_assets"]["images"] == 1 and reloaded["frame"] == 1, "one changed image, no tick")
            after = center_pixel(successful(agent, {"cmd": "observe", "image": image}))
            check(after[:3] == bytes((30, 30, 200)), "the reloaded image is drawn: %r" % after)
            successful(agent, {"cmd": "quit"})
        finally:
            agent.close()
    print("agent asset hot reload regressions passed")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
