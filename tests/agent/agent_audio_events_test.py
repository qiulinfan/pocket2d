#!/usr/bin/env python3
"""Audio and Music calls appear in a step's events (audio.play, music.play,
music.halt) without an audio device, with whether the clip resolves."""
import json
from pathlib import Path
import struct
import sys
import tempfile
from agent_protocol_test import Agent, check, successful


def write_wav(path):
    samples = b"\0\0" * 64
    header = b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16) + b"data" + struct.pack("<I", len(samples))
    path.write_bytes(header + samples)


def run(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-audio-") as directory:
        root = Path(directory)
        for folder in ("scenes", "component_types", "audio"):
            (root / folder).mkdir()
        write_wav(root / "audio/hit.wav")
        (root / "game.config").write_text('{"initial_scene":"main"}')
        (root / "scenes/main.scene").write_text(json.dumps({"actors": [
            {"name": "sound", "uid": 1, "components": {"s": {"type": "Sounder"}}}]}))
        (root / "component_types/Sounder.lua").write_text('''Sounder = { ticks = 0,
          OnUpdate = function(self)
            self.ticks = self.ticks + 1
            if self.ticks == 2 then Audio.Play(3, "hit", false); Audio.Play(4, "missing", true) end
            if self.ticks == 3 then Music.Play("hit", true) end
            if self.ticks == 4 then Music.Halt() end
          end }''')
        agent = Agent(executable, str(root))
        try:
            response = successful(agent, {"cmd": "step", "frames": 5, "events": True})
            events = [(e["type"], e["payload"]) for e in response["events"]]
            check(("audio.play", {"channel": 3, "clip": "hit", "loop": False, "found": True}) in events,
                  "a resolvable clip is reported: %r" % events)
            check(("audio.play", {"channel": 4, "clip": "missing", "loop": True, "found": False}) in events,
                  "a missing clip is reported as not found: %r" % events)
            check(("music.play", {"clip": "hit", "loop": True, "found": True}) in events, "music.play: %r" % events)
            check(("music.halt", {}) in events, "music.halt: %r" % events)
            filtered = successful(agent, {"cmd": "step", "frames": 1, "events": ["music.play"]})
            check(filtered["events"] == [], "type filters apply to engine events: %r" % filtered["events"])
            silent = successful(agent, {"cmd": "step", "frames": 1})
            check("events" not in silent, "no events without the request flag")
            successful(agent, {"cmd": "quit"})
            check(agent.process.wait(timeout=5) == 0, "clean quit")
        finally:
            agent.close()
    print("agent audio events passed")


if __name__ == "__main__":
    run(sys.argv[1])
