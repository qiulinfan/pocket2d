#!/usr/bin/env python3
"""Pocket2D netplay room server: room codes, UDP hole punching and relaying.

One UDP port (default 27777). Clients (src/engine/net/Netplay.cpp) send text:
  PK2 HOST <nonce> <lan ip:port>        -> PK2 ROOM <code>        (repeated until answered)
  PK2 JOIN <code> <nonce> <lan ip:port> -> both sides: PK2 PEER <public ip:port> <lan ip:port>
  PK2 KEEP <code>                        keeps the room and the NAT mapping alive
  PK2 LEAVE <code>
and, when hole punching fails, binary peer packets wrapped as b"PK2R" + packet,
which are forwarded to the other member of the sender's room.

Rooms expire after two minutes without traffic. No accounts, no game data is
inspected; the server only learns the two endpoints.

Usage: python3 room_server.py [--port 27777] [--bind 0.0.0.0]
"""
import argparse
import asyncio
import logging
import random
import string
import time

ROOM_TTL = 120.0
CODE_ALPHABET = "".join(c for c in string.ascii_uppercase + string.digits if c not in "O0I1")


class Room:
    def __init__(self, code, host, host_lan, nonce):
        self.code = code
        self.host = host
        self.host_lan = host_lan
        self.host_nonce = nonce
        self.guest = None
        self.guest_lan = None
        self.guest_nonce = None
        self.touched = time.monotonic()

    def other(self, addr):
        if addr == self.host:
            return self.guest
        if addr == self.guest:
            return self.host
        return None


def fmt(addr):
    return "%s:%d" % addr


class RoomServer(asyncio.DatagramProtocol):
    def __init__(self):
        self.rooms = {}          # code -> Room
        self.by_addr = {}        # addr -> Room
        self.host_by_nonce = {}  # (addr, nonce) -> code
        self.transport = None
        self.relayed_bytes = 0

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, text):
        self.transport.sendto(text.encode(), addr)

    def new_code(self):
        while True:
            code = "".join(random.choice(CODE_ALPHABET) for _ in range(4))
            if code not in self.rooms:
                return code

    def drop(self, room):
        self.rooms.pop(room.code, None)
        for addr in (room.host, room.guest):
            if addr and self.by_addr.get(addr) is room:
                del self.by_addr[addr]
        self.host_by_nonce = {k: v for k, v in self.host_by_nonce.items() if v != room.code}
        logging.info("room %s closed", room.code)

    def datagram_received(self, data, addr):
        if data[:4] == b"PK2R":
            room = self.by_addr.get(addr)
            if room:
                other = room.other(addr)
                if other:
                    room.touched = time.monotonic()
                    self.relayed_bytes += len(data)
                    self.transport.sendto(data, other)
            return
        try:
            parts = data.decode("ascii", "replace").split()
        except Exception:
            return
        if len(parts) < 2 or parts[0] != "PK2":
            return
        cmd = parts[1]
        if cmd == "HOST" and len(parts) >= 3:
            nonce = parts[2]
            lan = parts[3] if len(parts) > 3 else fmt(addr)
            code = self.host_by_nonce.get((addr, nonce))
            if code is None or code not in self.rooms:
                old = self.by_addr.get(addr)
                if old:
                    self.drop(old)
                code = self.new_code()
                room = Room(code, addr, lan, nonce)
                self.rooms[code] = room
                self.by_addr[addr] = room
                self.host_by_nonce[(addr, nonce)] = code
                logging.info("room %s opened by %s", code, fmt(addr))
            self.rooms[code].touched = time.monotonic()
            self.send(addr, "PK2 ROOM %s" % code)
        elif cmd == "JOIN" and len(parts) >= 4:
            code = parts[2].upper()
            nonce = parts[3]
            lan = parts[4] if len(parts) > 4 else fmt(addr)
            room = self.rooms.get(code)
            if room is None:
                self.send(addr, "PK2 ERR no such room")
                return
            if room.guest and room.guest != addr and room.guest_nonce != nonce:
                self.send(addr, "PK2 ERR room is full")
                return
            if room.guest is None:
                logging.info("room %s joined by %s", code, fmt(addr))
            room.guest, room.guest_lan, room.guest_nonce = addr, lan, nonce
            self.by_addr[addr] = room
            room.touched = time.monotonic()
            self.send(addr, "PK2 PEER %s %s" % (fmt(room.host), room.host_lan))
            self.send(room.host, "PK2 PEER %s %s" % (fmt(addr), lan))
        elif cmd == "KEEP" and len(parts) >= 3:
            room = self.rooms.get(parts[2].upper())
            if room and addr in (room.host, room.guest):
                room.touched = time.monotonic()
                if room.host == addr and room.guest is None:
                    self.send(addr, "PK2 ROOM %s" % room.code)
        elif cmd == "LEAVE" and len(parts) >= 3:
            room = self.rooms.get(parts[2].upper())
            if room and addr in (room.host, room.guest):
                self.drop(room)

    async def sweep(self):
        while True:
            await asyncio.sleep(10)
            now = time.monotonic()
            for room in list(self.rooms.values()):
                if now - room.touched > ROOM_TTL:
                    self.drop(room)


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=27777)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    server = RoomServer()
    await loop.create_datagram_endpoint(lambda: server, local_addr=(args.bind, args.port))
    logging.info("room server listening on %s:%d/udp", args.bind, args.port)
    await server.sweep()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
