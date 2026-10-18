#!/usr/bin/env python3
"""Lockstep netplay: two headless runtimes, through a local room server (hole
punching and forced relay) and directly, stay in step while each drives its own
player with the same local bindings."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from agent_protocol_test import Agent, check, successful

ACTIONS = {"version": 1, "actions": {
    "p1_move": {"type": "axis", "negative": ["a"], "positive": ["d"]},
    "p1_attack": {"type": "button", "keys": ["j"]},
    "p2_move": {"type": "axis", "negative": ["left"], "positive": ["right"]},
    "p2_attack": {"type": "button", "keys": ["l"]},
    "confirm": {"type": "button", "keys": ["return"]}}}

PROBE = '''NetProbe = {
  x1 = 0, x2 = 0, hits1 = 0, hits2 = 0, confirms = 0, ran = 0, hash = 0,
  OnAgentCommand = function(self, name, args)
    if name == "start" then
      Net.Configure{local_prefix = "p1_", sides = {"p1_", "p2_"}, shared = {"confirm"}, delay = 3,
        force_relay = args.relay or false}
      if args.mode == "host" then return Net.Host(args.server, "payload-42")
      elseif args.mode == "join" then return Net.Join(args.server, args.room)
      elseif args.mode == "hostdirect" then return Net.HostDirect(args.port, "lan-payload")
      else return Net.JoinDirect(args.address) end
    elseif name == "status" then return Net.Status()
    elseif name == "leave" then Net.Leave() return true end
  end,
  OnUpdate = function(self)
    if not Net.Running() then return end
    self.ran = self.ran + 1
    self.x1 = self.x1 + Input.GetActionValue("p1_move")
    self.x2 = self.x2 + Input.GetActionValue("p2_move")
    if Input.GetActionDown("p1_attack") then self.hits1 = self.hits1 + 1 end
    if Input.GetActionDown("p2_attack") then self.hits2 = self.hits2 + 1 end
    if Input.GetAction("confirm") then self.confirms = self.confirms + 1 end
    self.hash = (self.hash * 31 + math.floor(self.x1 * 1000) + 7 * math.floor(self.x2 * 1000) + self.hits1 * 13 + self.hits2) % 2147483647
    if self.ran % 30 == 0 then Net.Checksum(self.ran, self.hash) end
  end }'''


def write_project(root):
    (root / "scenes").mkdir()
    (root / "component_types").mkdir()
    (root / "game.config").write_text('{"initial_scene":"main"}')
    (root / "actions.json").write_text(json.dumps(ACTIONS))
    (root / "scenes/main.scene").write_text(json.dumps({"actors": [
        {"name": "probe", "uid": 1, "components": {"probe": {"type": "NetProbe"}}}]}))
    (root / "component_types/NetProbe.lua").write_text(PROBE)


def call(agent, name, args=None):
    r = successful(agent, {"cmd": "call", "name": name, "args": args or {}})
    return r["call"]["results"][0]["value"]


def props(response):
    actor = next(a for a in response["actors"] if a["name"] == "probe")
    return actor["components"][0]["properties"]


def both(a, b, req_a, req_b):
    out = [None, None]
    def go(i, agent, req):
        out[i] = successful(agent, req)
    ta = threading.Thread(target=go, args=(0, a, req_a))
    tb = threading.Thread(target=go, args=(1, b, req_b))
    ta.start(); tb.start(); ta.join(); tb.join()
    return out


def wait_running(a, b):
    for _ in range(200):
        both(a, b, {"cmd": "step", "frames": 1}, {"cmd": "step", "frames": 1})
        sa, sb = call(a, "status"), call(b, "status")
        if sa["state"] == "running" and sb["state"] == "running":
            return sa, sb
        check(sa["state"] != "error" and sb["state"] != "error", "no error: %s / %s" % (sa, sb))
        time.sleep(0.01)
    raise AssertionError("peers never started: %s / %s" % (call(a, "status"), call(b, "status")))


def play(a, b, label):
    # The host holds right and taps attack; the guest holds its own left (its p1 bindings)
    # and taps attack: on both machines the guest's input drives p2.
    for i in range(6):
        ra, rb = both(a, b,
                      {"cmd": "step", "frames": 20, "actions": {"p1_move": 1, "p1_attack": i % 2 == 0, "confirm": i == 3}},
                      {"cmd": "step", "frames": 20, "actions": {"p1_move": -1, "p1_attack": i % 2 == 1}})
    for _ in range(3):
        ra, rb = both(a, b, {"cmd": "step", "frames": 5, "actions": {}}, {"cmd": "step", "frames": 5, "actions": {}})
    # Lockstep lets one side run up to `delay` ticks ahead: compare at the same tick.
    for _ in range(20):
        fa, fb = call(a, "status")["frame"], call(b, "status")["frame"]
        if fa == fb:
            break
        behind = a if fa < fb else b
        r = successful(behind, {"cmd": "step", "actions": {}})
        if behind is a: ra = r
        else: rb = r
    ra, rb = successful(a, {"cmd": "observe"}), successful(b, {"cmd": "observe"})
    pa, pb = props(ra), props(rb)
    keys = ("x1", "x2", "hits1", "hits2", "confirms", "hash", "ran")
    sa, sb = call(a, "status"), call(b, "status")
    check(all(pa[k] == pb[k] for k in keys), label + ": both machines agree: %s / %s" % (pa, pb))
    check(pa["x1"] > 50 and pa["x2"] < -50, label + ": each player drives its own side: %s" % pa)
    check(pa["hits1"] == 3 and pa["hits2"] == 3 and pa["confirms"] > 0, label + ": edges and shared actions: %s" % pa)
    check(not sa["desync"] and not sb["desync"], label + ": no desync reported")
    check(sa["payload"] == sb["payload"] and sa["seed"] == sb["seed"], label + ": payload and seed shared")
    return sa, sb


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run(executable):
    server_port = free_port()
    server = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent.parent / "tools/room_server/room_server.py"),
                               "--port", str(server_port), "--bind", "127.0.0.1"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    try:
        for relay in (False, True):
            label = "relay" if relay else "punch"
            with tempfile.TemporaryDirectory(prefix="pocket-net-a-") as da, tempfile.TemporaryDirectory(prefix="pocket-net-b-") as db:
                write_project(Path(da)); write_project(Path(db))
                a, b = Agent(executable, da), Agent(executable, db)
                try:
                    srv = "127.0.0.1:%d" % server_port
                    check(call(a, "start", {"mode": "host", "server": srv, "relay": relay}), "host starts")
                    room = ""
                    for _ in range(100):
                        successful(a, {"cmd": "step"})
                        room = call(a, "status")["room"]
                        if room:
                            break
                        time.sleep(0.02)
                    check(len(room) == 4, label + ": room code issued")
                    check(call(b, "start", {"mode": "join", "server": srv, "room": room, "relay": relay}), "guest starts")
                    sa, sb = wait_running(a, b)
                    check(sa["side"] == 1 and sb["side"] == 2 and sb["payload"] == "payload-42", label + ": sides and payload")
                    sa, sb = play(a, b, label)
                    check(sa["relay"] == relay and sb["relay"] == relay, label + ": relay mode as requested")
                    call(a, "leave")
                    for _ in range(20):
                        successful(b, {"cmd": "step"})
                        if call(b, "status")["state"] == "disconnected":
                            break
                        time.sleep(0.02)
                    check(call(b, "status")["state"] == "disconnected", label + ": the guest sees the host leave")
                finally:
                    a.close(); b.close()
        with tempfile.TemporaryDirectory(prefix="pocket-net-a-") as da, tempfile.TemporaryDirectory(prefix="pocket-net-b-") as db:
            write_project(Path(da)); write_project(Path(db))
            a, b = Agent(executable, da), Agent(executable, db)
            try:
                port = free_port()
                check(call(a, "start", {"mode": "hostdirect", "port": port}), "direct host listens")
                check(call(b, "start", {"mode": "joindirect", "address": "127.0.0.1:%d" % port}), "direct guest connects")
                wait_running(a, b)
                play(a, b, "direct")
            finally:
                a.close(); b.close()
    finally:
        server.terminate()
        server.wait(timeout=10)
    print("netplay_test passed")


if __name__ == "__main__":
    run(sys.argv[1])
