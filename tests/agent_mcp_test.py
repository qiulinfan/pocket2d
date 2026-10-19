#!/usr/bin/env python3
"""MCP stdio server: legacy initialize and modern per-request clients drive the real runtime."""
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
from agent_protocol_test import check

MODERN = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
          "io.modelcontextprotocol/clientInfo": {"name": "pocket-test", "version": "1"},
          "io.modelcontextprotocol/clientCapabilities": {}}


class Client:
    def __init__(self, executable, project, *extra):
        self.process = subprocess.Popen(
            [executable, "--headless", "--mcp", "--project", project, "--seed", "42", *extra],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=lambda: [self.lines.put(l) for l in self.process.stdout] + [self.lines.put(None)],
                         daemon=True).start()
        threading.Thread(target=lambda: [self.errors.append(l) for l in self.process.stderr], daemon=True).start()
        self.next_id = 0

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def receive(self):
        try:
            line = self.lines.get(timeout=30)
        except queue.Empty:
            raise AssertionError("mcp response timed out: " + "".join(self.errors[-20:]))
        check(line is not None, "mcp server exited: " + "".join(self.errors[-20:]))
        message = json.loads(line)
        check(message.get("jsonrpc") == "2.0", "JSON-RPC 2.0 framing")
        return message

    def call(self, method, params=None, modern=False):
        self.next_id += 1
        params = dict(params or {})
        if modern:
            params["_meta"] = MODERN
        self.send({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params})
        message = self.receive()
        check(message["id"] == self.next_id, "response id matches")
        return message

    def close(self):
        self.process.stdin.close()
        try:
            check(self.process.wait(timeout=10) == 0, "server exits cleanly on EOF")
        finally:
            if self.process.poll() is None:
                self.process.kill()


def run(executable, project):
    client = Client(executable, project)
    try:
        # Legacy era: initialize handshake, then the usual methods.
        init = client.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "legacy", "version": "1"}})["result"]
        check(init["protocolVersion"] == "2025-06-18" and "tools" in init["capabilities"] and
              init["serverInfo"]["name"] == "pocket2d", "legacy initialize negotiates")
        client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        check(client.call("initialize", {"protocolVersion": "1999-01-01"})["result"]["protocolVersion"] == "2025-11-25",
              "unknown legacy versions get the newest legacy version")
        check(client.call("ping")["result"] == {}, "ping")
        tools = client.call("tools/list")["result"]["tools"]
        names = [tool["name"] for tool in tools]
        check(names == ["describe", "observe", "step", "run_until", "call", "reload_scripts", "reload_assets", "reset", "checkpoint", "restore"],
              "every command except quit is a tool: " + str(names))
        run_until = next(tool for tool in tools if tool["name"] == "run_until")
        check(run_until["inputSchema"]["required"] == ["max_frames", "until"] and
              run_until["inputSchema"]["$schema"].endswith("2020-12/schema"), "input schema from the command schema")
        check(next(t for t in tools if t["name"] == "observe")["annotations"]["readOnlyHint"] and
              not run_until["annotations"]["readOnlyHint"], "read-only hints follow access levels")

        stepped = client.call("tools/call", {"name": "step", "arguments": {"frames": 12, "keys": ["right"]}})["result"]
        check(not stepped["isError"] and stepped["structuredContent"]["frame"] == 12 and
              json.loads(stepped["content"][0]["text"]) == stepped["structuredContent"] and
              "resultType" not in stepped, "legacy tool call returns text and structured results")
        bad = client.call("tools/call", {"name": "step", "arguments": {"frames": 0}})["result"]
        check(bad["isError"] and not bad["structuredContent"]["ok"], "runtime rejections are tool errors")
        check(client.call("tools/call", {"name": "quit"})["error"]["code"] == -32602, "quit is not a tool")
        check(client.call("tools/call", {"name": "observe", "arguments": {"cmd": "step"}})["error"]["code"] == -32602,
              "reserved argument names are rejected")
        check(client.call("resources/list")["error"]["code"] == -32601, "unknown methods")
        client.send([{"jsonrpc": "2.0", "id": 99, "method": "ping"}])
        check(client.receive()["error"]["code"] == -32600, "batches are invalid requests")
        client.process.stdin.write("{not json\n"); client.process.stdin.flush()
        check(client.receive()["error"]["code"] == -32700, "parse errors")

        # Modern era: per-request metadata, discovery, resultType.
        discovered = client.call("server/discover", modern=True)["result"]
        check(discovered["supportedVersions"] == ["2026-07-28"] and discovered["resultType"] == "complete" and
              discovered["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "pocket2d", "server/discover")
        observed = client.call("tools/call", {"name": "observe"}, modern=True)["result"]
        check(observed["resultType"] == "complete" and observed["structuredContent"]["frame"] == 12,
              "modern calls share the same runtime")
        result = client.call("tools/call", {"name": "run_until", "arguments": {
            "max_frames": 30, "until": [{"actor_exists": "nobody"}]}}, modern=True)["result"]
        check(result["structuredContent"]["run_until"]["reason"] == "max_frames", "run_until over MCP")
        reset = client.call("tools/call", {"name": "reset", "arguments": {"seed": 5}}, modern=True)["result"]
        check(reset["structuredContent"]["frame"] == 0 and reset["structuredContent"]["seed"] == 5, "reset over MCP")
        client.next_id += 1
        client.send({"jsonrpc": "2.0", "id": client.next_id, "method": "tools/list",
                     "params": {"_meta": {**MODERN, "io.modelcontextprotocol/protocolVersion": "2030-01-01"}}})
        error = client.receive()["error"]
        check(error["code"] == -32022 and error["data"] == {"supported": ["2026-07-28"], "requested": "2030-01-01"},
              "unsupported modern versions are reported")
    finally:
        client.close()

    limited = Client(executable, project, "--mcp-access", "read")
    try:
        names = [tool["name"] for tool in limited.call("tools/list", modern=True)["result"]["tools"]]
        check(names == ["describe", "observe"], "access levels filter tools: " + str(names))
        check(limited.call("tools/call", {"name": "step"}, modern=True)["error"]["code"] == -32602,
              "disabled tools cannot be called")
    finally:
        limited.close()
    print("agent MCP server regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 3, "usage: agent_mcp_test.py RUNTIME FIXTURE_PROJECT")
    run(str(Path(sys.argv[1]).resolve()), str(Path(sys.argv[2]).resolve()))
