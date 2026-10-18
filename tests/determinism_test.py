#!/usr/bin/env python3
"""Cross-platform determinism: the determinism fixture hashes 2000 rounds of
float math, number formatting, string-keyed table order and sorting, one hash
per category, and every category must equal the value every platform produces
(recorded on macOS arm64 with Apple clang; the Linux x86_64 runtime kit, GCC 14
/ glibc, matches, and Windows x64 with MSVC and llvm-mingw matched except log2
before math.log stopped depending on Lua's C89 setting). A different hash
names the operation that would desync machines in netplay or replays."""
import sys
from agent_protocol_test import Agent, check, successful

EXPECTED = {
    "acos": "74d61f85", "asin": "b9cec9e1", "atan": "2d0b0775", "atan2": "adad0100", "cos": "85faf9aa",
    "exp": "9a406647", "floor": "5c7a571b", "fmod": "ee8553f6", "log": "fd596fdb", "log10": "8c112864",
    "log2": "2fc46812", "pairs": "7d15dbb4", "pow137": "87f6f861", "pow2x": "089a7bb7", "sin": "dc0cced2",
    "sort": "f4db000d", "sq": "31bc61e1", "sqrt": "0a3b0782", "tan": "9009ee6b", "tostr": "14316200",
    "tostr_e15": "0ed637fc", "tostr_e7": "5ba9d691", "x": "76736eb5",
}


def run(executable, project):
    agent = Agent(executable, project)
    try:
        r = successful(agent, {"cmd": "call", "name": "digest", "args": {}})
        value = next(x["value"] for x in r["call"]["results"] if x.get("value"))
        got = value["cats"]
        wrong = {k: (got.get(k), v) for k, v in EXPECTED.items() if got.get(k) != v}
        check(not wrong, "categories differing (got, expected): %s" % wrong)
    finally:
        agent.close()
    print("determinism_test passed")


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2])
