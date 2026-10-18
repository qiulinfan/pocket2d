#!/usr/bin/env python3
"""Cross-platform determinism: the determinism fixture's digest of float
math, number formatting, table order and sorting must equal the value every
platform produces (recorded on macOS arm64 with Apple clang and matched by
the Linux x86_64 runtime kit, GCC 14 / glibc, under emulation). A different
digest means scripts would desync between machines in netplay or replays."""
import sys
from agent_protocol_test import Agent, check, successful

EXPECTED = "619a79f9"


def run(executable, project):
    agent = Agent(executable, project)
    try:
        r = successful(agent, {"cmd": "call", "name": "digest", "args": {}})
        value = next(x["value"] for x in r["call"]["results"] if x.get("value"))
        print(value)
        check(value["values"] > 10000, "the probe ran")
        check(value["digest"] == EXPECTED, "digest %s, expected %s (%s)" % (value["digest"], EXPECTED, value["sample"]))
    finally:
        agent.close()
    print("determinism_test passed")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
