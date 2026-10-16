#!/usr/bin/env python3
"""Headless visual observations: software-rendered PNG frames over the agent protocol."""
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from agent_protocol_test import check
from agent_savegame_test import SaveAgent
from agent_protocol_test import successful


def decode_png(data):
    """RGBA bytes of an 8-bit RGB/RGBA PNG (standard library only)."""
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature")
    i, idat = 8, b""
    while i < len(data):
        length = struct.unpack(">I", data[i:i + 4])[0]
        kind, chunk = data[i + 4:i + 8], data[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", chunk[:10])
            check(depth == 8 and color in (2, 6), "8-bit RGB(A) PNG")
        elif kind == b"IDAT":
            idat += chunk
        i += 12 + length
    bpp = 4 if color == 6 else 3
    raw, stride, out, previous, at = zlib.decompress(idat), width * bpp, bytearray(), bytearray(width * bpp), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = previous[x]
            c = previous[x - bpp] if x >= bpp else 0
            if kind == 1: line[x] = (line[x] + a) & 255
            elif kind == 2: line[x] = (line[x] + b) & 255
            elif kind == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        previous = line
    rgb = [bytes(out[i:i + 3]) for i in range(0, len(out), bpp)]
    return width, height, rgb


def image_of(response):
    image = response["image"]
    check(image["format"] == "png" and image["encoding"] == "base64", "image encoding")
    return image, base64.b64decode(image["data"])


def without_image(response):
    return {key: value for key, value in response.items() if key not in ("image", "id", "diagnostics")}


def run(executable, project):
    with tempfile.TemporaryDirectory(prefix="pocket-image-") as directory:
        root = Path(directory)
        agent = SaveAgent(executable, str(project), str(root / "saves"))
        try:
            stepped = successful(agent, {"cmd": "step", "frames": 31, "image": {"width": 960, "height": 720}})
            image, png = image_of(stepped)
            width, height, headless = decode_png(png)
            check((width, height) == (960, 720) == (image["width"], image["height"]), "requested size")
            check(len(set(headless)) > 8, "the frame shows the scene, not a cleared target")
            again = successful(agent, {"cmd": "observe", "image": {"width": 960, "height": 720}})
            check(image_of(again)[1] == png, "rendering the same frame twice is identical")
            small = image_of(successful(agent, {"cmd": "observe", "image": {"width": 320, "height": 240}}))[1]
            check(decode_png(small)[:2] == (320, 240), "output is scaled to the requested size")
            for bad in [{"width": 0, "height": 10}, {"width": 4000, "height": 10}, {"width": 10}]:
                response = agent.request({"cmd": "observe", "image": bad})
                check(not response["ok"] and response["code"] == "invalid_argument", "rejects image " + json.dumps(bad))
            looked = [without_image(successful(agent, {"cmd": "step", "frames": 5, "keys": ["right"],
                                                       "image": {"width": 64, "height": 48}})) for _ in range(4)]
        finally:
            agent.close()

        agent = SaveAgent(executable, str(project), str(root / "saves"))
        try:
            successful(agent, {"cmd": "step", "frames": 31})
            blind = [without_image(successful(agent, {"cmd": "step", "frames": 5, "keys": ["right"]})) for _ in range(4)]
            check(looked == blind, "observing images never changes the simulation")
        finally:
            agent.close()

        # Graphical mode with the software renderer draws the same pixels.
        capture = root / "graphical.png"
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           POCKET_RENDER_DRIVER="software", POCKET_RENDER_VSYNC="0")
        result = subprocess.run([executable, "--project", str(project), "--save-dir", str(root / "saves"),
                                 "--frames", "31", "--capture", str(capture)],
                                env=environment, capture_output=True, text=True, timeout=60)
        check(result.returncode == 0, "graphical capture failed: " + result.stderr[-2000:])
        graphical = decode_png(capture.read_bytes())
        differing = sum(1 for a, b in zip(graphical[2], headless) if a != b)
        check(graphical[:2] == (960, 720) and differing == 0,
              "headless and graphical software frames match pixel for pixel (%d differ)" % differing)
    print("agent headless image observation regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_image_test.py RUNTIME TWO_WORLD_VILLAGE")
    run(str(Path(sys.argv[1]).resolve()), Path(sys.argv[2]).resolve())
