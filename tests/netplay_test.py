#!/usr/bin/env python3
"""Netplay: two headless runtimes, each driving its own player with the same
local bindings, must agree on every synced value, edge and shared action.

Links: a local room server (hole punching and forced relay), direct
connections found by LAN discovery, and connection codes through a local STUN
server; a host behind a slow direct path plays over the faster relay, with the
ping and loss measured. Sync: lockstep, and rollback (with corrections counted). A sync test
session must pass with all state tracked and catch state that is not. Sounds
across corrections: each is played once, those a correction reveals are played
late, mispredicted ones are cancelled, and both machines end up with the same
sounds."""
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
  x1 = 0, x2 = 0, hits1 = 0, hits2 = 0, confirms = 0, ran = 0, hash = 0, leaky = false,
  OnAgentCommand = function(self, name, args)
    if name == "start" then
      Net.Configure{local_prefix = "p1_", sides = {"p1_", "p2_"}, shared = {"confirm"},
        mode = args.rollback and "rollback" or "lockstep", delay = args.delay or 3,
        force_relay = args.relay or false, game = "netprobe", name = args.name,
        discovery_port = args.dport, stun = args.stun}
      self.leaky = args.leaky or false
      -- This machine's own (a resync leaves it), and state that only a portable
      -- export carries well: closures made at run time, a table keyed by a table.
      self.mine = {side = args.mode, ticks = 0}
      Net.Local(self.mine)
      self.fns, self.by = {}, {}
      self.key = {name = "key"}
      Net.Track(self)
      if args.mode == "host" then return Net.Host(args.server, "payload-42")
      elseif args.mode == "join" then return Net.Join(args.server, args.room)
      elseif args.mode == "hostdirect" then return Net.HostDirect(args.port, "lan-payload")
      elseif args.mode == "joindirect" then return Net.JoinDirect(args.address)
      elseif args.mode == "codehost" then return Net.CodeHost("code-payload")
      elseif args.mode == "codejoin" then return Net.CodeJoin(args.code)
      elseif args.mode == "synctest" then return Net.SyncTest(args.frames or 7) end
    elseif name == "codeconnect" then return Net.CodeConnect(args.code)
    elseif name == "discover" then
      Net.Configure{game = "netprobe", discovery_port = args.dport}
      Net.Discover(true) return true
    elseif name == "hosts" then return Net.Hosts()
    elseif name == "status" then return Net.Status()
    elseif name == "perturb" then
      -- Desync recovery: this machine's state changes inside tick `at` (each time it is simulated).
      if not PERTURB then PERTURB = {}; Net.Static(PERTURB) end
      PERTURB[self.ran + (args.after or 10)] = args.by or 1000
      return true
    elseif name == "mine" then return self.mine
    elseif name == "leave" then Net.Leave() return true end
  end,
  OnUpdate = function(self)
    if not Net.Running() then return end
    self.ran = self.ran + 1
    self.x1 = self.x1 + Input.GetActionValue("p1_move")
    self.x2 = self.x2 + Input.GetActionValue("p2_move")
    if Input.GetActionDown("p1_attack") then self.hits1 = self.hits1 + 1; Audio.Play(1, "hit1", false) end
    if Input.GetActionDown("p2_attack") then self.hits2 = self.hits2 + 1; Audio.Play(2, "hit2", false) end
    -- While attack is held: a prediction that holds it too long plays sounds a correction takes back.
    if self.ran % 4 == 0 and Input.GetAction("p1_attack") then Audio.Play(3, "buzz1", false) end
    if self.ran % 4 == 0 and Input.GetAction("p2_attack") then Audio.Play(4, "buzz2", false) end
    if Input.GetAction("confirm") then self.confirms = self.confirms + 1 end
    self.hash = (self.hash * 31 + math.floor(self.x1 * 1000) + 7 * math.floor(self.x2 * 1000) + self.hits1 * 13 + self.hits2) % 2147483647
    if self.leaky then
      -- State the snapshots leave out (a table declared static, yet changed) that
      -- feeds tracked state: a sync test must notice.
      if not LEAK then LEAK = {n = 0}; Net.Static(LEAK) end
      LEAK.n = LEAK.n + 1
      self.hash = (self.hash + LEAK.n) % 2147483647
    end
    if PERTURB and PERTURB[self.ran] then
      self.x1 = self.x1 + PERTURB[self.ran]
      -- A closure of code the other machine never ran: a resync from here sends that code.
      local n = PERTURB[self.ran]
      self.extra = function() return n % 97 end
    end
    if self.extra then self.hash = (self.hash + self.extra()) % 2147483647 end
    self.mine.ticks = self.mine.ticks + 1
    if self.ran % 20 == 0 then
      local k = self.ran
      self.fns[#self.fns + 1] = function() return k end
      if #self.fns > 5 then table.remove(self.fns, 1) end
      self.by[self.key] = (self.by[self.key] or 0) + 1
    end
    local sum = 0
    for _, f in ipairs(self.fns) do sum = sum + f() end
    self.hash = (self.hash + sum + (self.by[self.key] or 0)) % 2147483647
    if self.ran % 30 == 0 then Net.Checksum(self.ran, self.hash) end
  end }'''

STUN_MAGIC = 0x2112A442


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


SOUND_EVENTS = ["audio.play", "audio.cancel"]


def stepped(agent, req):
    """A request whose step reports the sounds; the agent keeps them in `sounds`."""
    if req.get("cmd") == "step":
        req = dict(req, events=SOUND_EVENTS)
    response = successful(agent, req)
    if not hasattr(agent, "sounds"):
        agent.sounds, agent.logs = [], []
    agent.logs += [d.get("message", "") for d in response.get("diagnostics", [])]
    agent.sounds += [(e["type"], e["payload"]) for e in response.get("events", [])]
    return response


def final_sounds(agent, label, before):
    """The sounds of ticks before `before` that stayed: played and not cancelled, each once."""
    plays, cancels = {}, set()
    for kind, p in getattr(agent, "sounds", []):
        key = (p["tick"], p["clip"], p["nth"])
        if kind == "audio.play":
            check(key not in plays or key in cancels, label + ": a sound is played once: %s" % (key,))
            cancels.discard(key)
            plays[key] = p
        else:
            check(key in plays, label + ": only played sounds are cancelled: %s" % (key,))
            cancels.add(key)
    kept = {k for k in plays if k not in cancels and k[0] < before}
    late = sum(1 for k, p in plays.items() if p["late"] > 0)
    return kept, late, len(cancels)


def both(a, b, req_a, req_b):
    out = [None, None]
    def go(i, agent, req):
        out[i] = stepped(agent, req)
    ta = threading.Thread(target=go, args=(0, a, req_a))
    tb = threading.Thread(target=go, args=(1, b, req_b))
    ta.start(); tb.start(); ta.join(); tb.join()
    return out


def wait_running(a, b):
    for _ in range(300):
        both(a, b, {"cmd": "step", "frames": 1}, {"cmd": "step", "frames": 1})
        sa, sb = call(a, "status"), call(b, "status")
        if sa["state"] == "running" and sb["state"] == "running":
            return sa, sb
        check(sa["state"] != "error" and sb["state"] != "error", "no error: %s / %s" % (sa, sb))
        time.sleep(0.01)
    raise AssertionError("peers never started: %s / %s" % (call(a, "status"), call(b, "status")))


def play(a, b, label, rollback=False):
    # The host holds right and taps attack; the guest holds its own left (its p1 bindings)
    # and taps attack: on both machines the guest's input drives p2.
    for i in range(6):
        ra, rb = both(a, b,
                      {"cmd": "step", "frames": 20, "actions": {"p1_move": 1, "p1_attack": i % 2 == 0, "confirm": i == 3}},
                      {"cmd": "step", "frames": 20, "actions": {"p1_move": -1, "p1_attack": i % 2 == 1}})
    for _ in range(3):
        ra, rb = both(a, b, {"cmd": "step", "frames": 5, "actions": {}}, {"cmd": "step", "frames": 5, "actions": {}})
    # One side may run ahead of the other: compare at the same tick, once its
    # inputs are final everywhere (rollback corrects a few ticks back).
    for _ in range(60):
        sa, sb = call(a, "status"), call(b, "status")
        if sa["frame"] == sb["frame"] and (not rollback or sa["frame"] > 0):
            if not rollback:
                break
            both(a, b, {"cmd": "step", "frames": 1, "actions": {}}, {"cmd": "step", "frames": 1, "actions": {}})
            sa2, sb2 = call(a, "status"), call(b, "status")
            if sa2["frame"] == sb2["frame"]:
                break
            continue
        behind = a if sa["frame"] < sb["frame"] else b
        stepped(behind, {"cmd": "step", "actions": {}})
    ra, rb = successful(a, {"cmd": "observe"}), successful(b, {"cmd": "observe"})
    pa, pb = props(ra), props(rb)
    keys = ("x1", "x2", "hits1", "hits2", "confirms", "hash", "ran")
    sa, sb = call(a, "status"), call(b, "status")
    check(all(pa[k] == pb[k] for k in keys), label + ": both machines agree: %s / %s" % (pa, pb))
    check(pa["x1"] > 50 and pa["x2"] < -50, label + ": each player drives its own side: %s" % pa)
    check(pa["hits1"] == 3 and pa["hits2"] == 3 and pa["confirms"] > 0, label + ": edges and shared actions: %s" % pa)
    check(not sa["desync"] and not sb["desync"], label + ": no desync reported: %s / %s" % (sa, sb))
    check(sa["payload"] == sb["payload"] and sa["seed"] == sb["seed"], label + ": payload and seed shared")
    check(sa["mode"] == sb["mode"] == ("rollback" if rollback else "lockstep"), label + ": sync mode " + sa["mode"])
    # Sounds of the ticks both have simulated with the real inputs.
    kept_a, late_a, cancels_a = final_sounds(a, label + " host", sa["frame"] - 1)
    kept_b, late_b, cancels_b = final_sounds(b, label + " guest", sa["frame"] - 1)
    hits = {"hit1": 0, "hit2": 0}
    for _, clip, _ in kept_a:
        hits[clip] = hits.get(clip, 0) + 1
    check(kept_a == kept_b and hits["hit1"] == pa["hits1"] and hits["hit2"] == pa["hits2"],
          label + ": both machines heard the same sounds (%d; %s)" % (len(kept_a), hits))
    if rollback:
        print("%s: sounds played late %d / %d, cancelled %d / %d" % (label, late_a, late_b, cancels_a, cancels_b))
    return sa, sb


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class StunServer:
    """Answers STUN binding requests with the sender's address."""

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.port = self.sock.getsockname()[1]
        self.running = True
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if len(data) < 20 or int.from_bytes(data[4:8], "big") != STUN_MAGIC:
                continue
            ip = int.from_bytes(socket.inet_aton(addr[0]), "big") ^ STUN_MAGIC
            port = addr[1] ^ (STUN_MAGIC >> 16)
            attr = bytes([0, 0x20, 0, 8, 0, 1]) + port.to_bytes(2, "big") + ip.to_bytes(4, "big")
            self.sock.sendto(bytes([1, 1, 0, 12]) + data[4:20] + attr, addr)

    def close(self):
        self.running = False


class SlowDirect:
    """A NAT in front of the host whose direct path is slow. The host talks to
    the room server through `port` (socket A); the server and the guest see
    the host at socket B, and the host sees the guest at B as well. Direct
    packets either way are held for `lag` seconds; server and relay traffic
    passes at once, so the relay is the faster path."""

    def __init__(self, server, lag):
        self.server, self.lag = server, lag
        self.a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for sock in (self.a, self.b):
            sock.bind(("127.0.0.1", 0))
            sock.setblocking(False)
        self.port = self.a.getsockname()[1]
        self.public = b"127.0.0.1:%d" % self.b.getsockname()[1]
        self.host = self.guest = None
        self.pending = []
        self.running = True
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        import select
        while self.running:
            ready, _, _ = select.select([self.a, self.b], [], [], 0.002)
            later = time.monotonic() + self.lag
            for sock in ready:
                try:
                    data, addr = sock.recvfrom(4096)
                except BlockingIOError:
                    continue
                if sock is self.a:  # the host to the server
                    self.host = addr
                    if data.startswith(b"PK2 HOST "):
                        data = b" ".join(data.split(b" ")[:3] + [self.public])
                    self.pending.append((0, self.b, data, self.server))
                elif addr == self.server:
                    if data.startswith(b"PK2 PEER "):
                        ip, port = data.split(b" ")[2].decode().split(":")
                        self.guest = (ip, int(port))
                        data = b"PK2 PEER " + self.public + b" " + self.public
                    self.pending.append((0, self.a, data, self.host))
                elif addr == self.host:  # direct, to the guest
                    self.pending.append((later, self.b, data, self.guest))
                else:  # direct, from the guest
                    self.pending.append((later, self.b, data, self.host))
            now = time.monotonic()
            due = [x for x in self.pending if x[0] <= now]
            self.pending = [x for x in self.pending if x[0] > now]
            for _, sock, data, to in due:
                if to:
                    sock.sendto(data, to)

    def close(self):
        self.running = False


def relay_choice(executable, server_port):
    """The host times the direct path and the relay and plays over the faster."""
    proxy = SlowDirect(("127.0.0.1", server_port), 0.08)
    a, b = pair(executable)
    try:
        check(call(a, "start", {"mode": "host", "server": "127.0.0.1:%d" % proxy.port, "rollback": True, "delay": 0}),
              "host starts behind the slow proxy")
        room = ""
        for _ in range(100):
            successful(a, {"cmd": "step"})
            room = call(a, "status")["room"]
            if room:
                break
            time.sleep(0.02)
        check(call(b, "start", {"mode": "join", "server": "127.0.0.1:%d" % server_port, "room": room}), "guest starts")
        sa, sb = wait_running(a, b)
        check(sa["direct_ms"] >= 150 and 0 <= sa["relay_ms"] < 100, "the host timed both paths: direct %d ms, relay %d ms"
              % (sa["direct_ms"], sa["relay_ms"]))
        check(sa["relay"] and sb["relay"], "both play over the faster relay")
        # The loss counts probes old enough to have been answered (1.5 s, 8 at least).
        for _ in range(80):
            both(a, b, {"cmd": "step", "frames": 5}, {"cmd": "step", "frames": 5})
            time.sleep(0.05)
        sa, sb = call(a, "status"), call(b, "status")
        check(0 <= sa["ping"] < 100 and 0 <= sa["loss"] <= 0.1 and 0 <= sb["loss"] <= 0.1,
              "ping %d ms and loss %.2f / %.2f over the relay" % (sa["ping"], sa["loss"], sb["loss"]))
        print("relay choice: direct %d ms, relay %d ms, ping %d ms, loss %.2f" % (
            sa["direct_ms"], sa["relay_ms"], sa["ping"], sa["loss"]))
    finally:
        a.close(); b.close()
        proxy.close()


def pair(executable):
    da, db = tempfile.mkdtemp(prefix="pocket-net-a-"), tempfile.mkdtemp(prefix="pocket-net-b-")
    write_project(Path(da)); write_project(Path(db))
    return Agent(executable, da), Agent(executable, db)


def leave_and_check(a, b, label):
    call(a, "leave")
    for _ in range(20):
        successful(b, {"cmd": "step"})
        if call(b, "status")["state"] == "disconnected":
            break
        time.sleep(0.02)
    check(call(b, "status")["state"] == "disconnected", label + ": the guest sees the host leave")


def room_server(executable, server_port, relay, rollback):
    label = ("relay" if relay else "punch") + (" rollback" if rollback else "")
    a, b = pair(executable)
    try:
        srv = "127.0.0.1:%d" % server_port
        check(call(a, "start", {"mode": "host", "server": srv, "relay": relay, "rollback": rollback, "delay": 2}), "host starts")
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
        sa, sb = play(a, b, label, rollback)
        check(sa["relay"] == relay and sb["relay"] == relay, label + ": relay mode as requested")
        check(sa["loss"] <= 0 and sb["loss"] <= 0, label + ": no loss on this machine (%s / %s)" % (sa["loss"], sb["loss"]))
        if rollback:
            check(sa["rollbacks"] + sb["rollbacks"] > 0, label + ": predictions were corrected (%d / %d rollbacks)"
                  % (sa["rollbacks"], sb["rollbacks"]))
        leave_and_check(a, b, label)
    finally:
        a.close(); b.close()


def discovery(executable):
    a, b = pair(executable)
    try:
        dport, port = free_port(), free_port()
        check(call(a, "start", {"mode": "hostdirect", "port": port, "dport": dport, "name": "probe-host"}), "direct host listens")
        lan = call(a, "status")["lan"]
        check(lan.endswith(":%d" % port), "the direct host knows its LAN address: " + lan)
        check(call(b, "discover", {"dport": dport}), "discovery starts")
        hosts = []
        for _ in range(100):
            both(a, b, {"cmd": "step"}, {"cmd": "step"})
            hosts = call(b, "hosts") or []
            if hosts:
                break
            time.sleep(0.03)
        check(len(hosts) >= 1 and hosts[0]["name"] == "probe-host" and hosts[0]["payload"] == "lan-payload",
              "LAN discovery finds the host: %s" % hosts)
        found = hosts[0]["address"]
        check(found.endswith(":%d" % port), "the found address carries the game port: " + found)
        check(call(b, "start", {"mode": "joindirect", "address": found}), "the guest joins the found host")
        wait_running(a, b)
        play(a, b, "direct")
    finally:
        a.close(); b.close()


def codes(executable):
    stun = StunServer()
    a, b = pair(executable)
    try:
        servers = ["127.0.0.1:%d" % stun.port]
        check(call(a, "start", {"mode": "codehost", "stun": servers, "rollback": True, "delay": 1}), "code host starts")
        code = ""
        for _ in range(100):
            successful(a, {"cmd": "step"})
            code = call(a, "status")["code"]
            if code:
                break
            time.sleep(0.02)
        check(len(code) == 23, "the host gets a connection code: " + code)
        check(call(b, "start", {"mode": "codejoin", "code": code, "stun": servers}), "the guest uses the code")
        guest_code = ""
        for _ in range(100):
            both(a, b, {"cmd": "step"}, {"cmd": "step"})
            guest_code = call(b, "status")["code"]
            if guest_code:
                break
            time.sleep(0.02)
        check(len(guest_code) == 23, "the guest gets its own code: " + guest_code)
        check(call(a, "codeconnect", {"code": guest_code}), "the host takes the guest's code")
        sa, sb = wait_running(a, b)
        check(sa["link"] == "code" and sa["nat"] == "unknown" or sa["nat"] in ("cone", "symmetric"), "link: %s" % sa)
        play(a, b, "code rollback", rollback=True)
    finally:
        a.close(); b.close()
        stun.close()


def resync(executable, rollback):
    """A desync (one machine's state changes inside a tick) is caught and repaired: the
    host's state reaches the guest, both agree afterwards, local tables stay each one's."""
    label = "resync" + (" rollback" if rollback else "")
    a, b = pair(executable)
    try:
        port = free_port()
        check(call(a, "start", {"mode": "hostdirect", "port": port, "rollback": rollback, "delay": 2}), "direct host listens")
        check(call(b, "start", {"mode": "joindirect", "address": "127.0.0.1:%d" % port, "rollback": rollback}), "guest joins")
        wait_running(a, b)
        for i in range(4):
            both(a, b, {"cmd": "step", "frames": 15, "actions": {"p1_move": 1, "p1_attack": i % 2 == 0}},
                 {"cmd": "step", "frames": 15, "actions": {"p1_move": -1}})
        for who, name in ((a, "host"), (b, "guest")):
            before = call(b, "status")["resyncs"]
            check(call(who, "perturb", {"after": 5}), label + ": " + name + " perturbed")
            seen = False
            for _ in range(200):
                both(a, b, {"cmd": "step", "frames": 2, "actions": {"p1_move": 1}}, {"cmd": "step", "frames": 2, "actions": {}})
                sa, sb = call(a, "status"), call(b, "status")
                seen = seen or sa["desync"] or sb["desync"]
                if sb["resyncs"] > before and sa["resyncs"] > before and not sa["desync"] and not sb["desync"]:
                    break
                time.sleep(0.005)
            check(seen, label + ": " + name + " changed: the desync is seen")
            check(sb["resyncs"] > before and not sb["desync"] and not sb["resync_error"],
                  label + ": " + name + " changed: the guest loaded the host's state (%s)" % sb)
            for _ in range(5):
                both(a, b, {"cmd": "step", "frames": 5, "actions": {}}, {"cmd": "step", "frames": 5, "actions": {}})
            for _ in range(60):
                sa, sb = call(a, "status"), call(b, "status")
                if sa["frame"] == sb["frame"]:
                    break
                stepped(a if sa["frame"] < sb["frame"] else b, {"cmd": "step", "actions": {}})
            pa, pb = props(successful(a, {"cmd": "observe"})), props(successful(b, {"cmd": "observe"}))
            keys = ("x1", "x2", "hits1", "hits2", "hash", "ran")
            check(all(pa[k] == pb[k] for k in keys), label + ": " + name + " changed: both agree afterwards: %s / %s" % (pa, pb))
            ma, mb = call(a, "mine"), call(b, "mine")
            check(ma["side"] == "hostdirect" and mb["side"] == "joindirect" and mb["ticks"] > 0,
                  label + ": local tables stay each machine's own: %s / %s" % (ma, mb))
            if name == "host":
                # The host's state holds a closure of code the guest never ran: it asks for that code.
                check(any("code the guest lacks" in m for m in a.logs), label + ": the guest got code it lacked")
            print("%s: %s changed, tick %d resynced (%d bytes)" % (label, name, sb["resync_frame"], sb["resync_bytes"]))
    finally:
        a.close(); b.close()


def sync_test(executable, leaky):
    a, _ = pair(executable)
    try:
        check(call(a, "start", {"mode": "synctest", "frames": 6, "leaky": leaky}), "sync test starts")
        for i in range(10):
            stepped(a, {"cmd": "step", "frames": 10, "actions": {"p1_move": 1 if i % 2 else -1, "p1_attack": i % 3 == 0,
                                                                  "p2_attack": i % 4 == 1}})
        s = call(a, "status")
        p = props(successful(a, {"cmd": "observe"}))
        check(s["rollbacks"] > 50 and s["resimulated"] >= 6 * s["rollbacks"], "sync test resimulates: %s" % s)
        kept, late, cancels = final_sounds(a, "sync test", s["frame"])
        check(late == 0 and cancels == 0 and sum(1 for k in kept if k[1] == "hit1") == p["hits1"],
              "resimulated ticks neither repeat nor take back sounds (%d)" % len(kept))
        if leaky:
            check(s["desync"], "the sync test catches state outside the tracked roots")
        else:
            check(not s["desync"] and p["ran"] == 100 and p["hits1"] == 4, "the sync test passes: %s %s" % (s, p))
    finally:
        a.close()


def run(executable):
    server_port = free_port()
    server = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parent.parent / "tools/room_server/room_server.py"),
                               "--port", str(server_port), "--bind", "127.0.0.1"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    try:
        room_server(executable, server_port, relay=False, rollback=False)
        room_server(executable, server_port, relay=True, rollback=False)
        room_server(executable, server_port, relay=False, rollback=True)
        relay_choice(executable, server_port)
        discovery(executable)
        codes(executable)
        resync(executable, rollback=True)
        resync(executable, rollback=False)
        sync_test(executable, leaky=False)
        sync_test(executable, leaky=True)
    finally:
        server.terminate()
        server.wait(timeout=10)
    print("netplay_test passed")


if __name__ == "__main__":
    run(sys.argv[1])
