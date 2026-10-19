#!/usr/bin/env python3
"""Real supervised workers: Lua quotas, native blocking, recovery and transport bounds."""
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def kill_test_process(process):
    # Failed tests must not leave an intentionally looping worker behind.
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    process.wait(timeout=5)


def arguments(executable, project, timeout=1500, heap=8 * 1024 * 1024):
    return [executable, "--headless", "--agent", "--project", str(project),
            "--instruction-budget", "20000", "--lua-heap-budget", str(heap),
            "--request-timeout-ms", str(timeout)]


class Agent:
    def __init__(self, executable, project, timeout=1500, heap=8 * 1024 * 1024):
        self.process = subprocess.Popen(
            arguments(executable, project, timeout, heap), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
            start_new_session=(os.name == "posix"))
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=self.read_stdout, daemon=True).start()
        threading.Thread(target=self.read_stderr, daemon=True).start()

    def read_stdout(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def read_stderr(self):
        for line in self.process.stderr:
            self.errors.append(line)
            if len(self.errors) > 50:
                self.errors.pop(0)

    def request(self, command, raw=False):
        self.process.stdin.write((command if raw else json.dumps(command)) + "\n")
        self.process.stdin.flush()
        try:
            line = self.lines.get(timeout=7)
        except queue.Empty:
            raise AssertionError("supervisor failed to bound request: " + "".join(self.errors))
        check(line is not None, "supervisor exited without response: " + "".join(self.errors))
        response = json.loads(line)
        check(isinstance(response.get("ok"), bool), "response must have boolean ok")
        check("_pocket_request_id" not in response and "_initial_observation" not in response,
              "private worker transport must not leak into public responses")
        return response

    def quit(self):
        check(self.request({"cmd": "quit"})["ok"], "quit must work with an unhealthy worker")
        check(self.process.wait(timeout=5) == 0, "supervisor quit failed")
        check(self.lines.get(timeout=5) is None, "unsolicited stdout after quit")

    def close(self):
        if self.process.poll() is None:
            kill_test_process(self.process)


def project_at(root, script):
    (root / "component_types").mkdir()
    (root / "scenes").mkdir()
    (root / "game.config").write_text('{"initial_scene":"test"}')
    (root / "scenes" / "test.scene").write_text(json.dumps({"actors": [
        {"uid": 1, "name": "probe", "components": {"logic": {"type": "Probe"}}}]}))
    write_probe(root, script)


def write_probe(root, body):
    (root / "component_types" / "Probe.lua").write_text(
        "Probe = {ticks=0, OnUpdate=function(self) " + body + " end}")


def successful(agent, command):
    response = agent.request(command)
    check(response["ok"], str(response))
    return response


def verify_recovery(agent, root):
    rejected = agent.request({"cmd": "observe"})
    check(not rejected["ok"] and rejected.get("requires_reset"), "poisoned worker must require reset")
    description = successful(agent, {"cmd": "describe"})
    check(description.get("requires_reset") is True, "cached discovery must mark unavailable runtime")
    write_probe(root, "self.ticks = self.ticks + 1")
    reset = successful(agent, {"cmd": "reset", "seed": 17})
    check(reset["frame"] == 0 and reset["seed"] == 17, "reset must start a fresh seeded worker")
    stepped = successful(agent, {"cmd": "step"})
    check(stepped["frame"] == 1, "fresh worker must step after failure")
    props = stepped["actors"][0]["components"][0]["properties"]
    check(props["ticks"] == 1, "fresh worker must execute repaired source")


def failing_callback(executable, body, cause, timeout=1500, heap=8 * 1024 * 1024):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-budget-") as directory:
        root = Path(directory)
        project_at(root, body)
        agent = Agent(executable, root, timeout, heap)
        try:
            successful(agent, {"cmd": "observe"})
            begin = time.monotonic()
            response = agent.request({"cmd": "step", "frames": 1})
            elapsed = time.monotonic() - begin
            check(not response["ok"] and response.get("requires_reset"),
                  "unsafe request must fail and invalidate worker: " + str(response))
            check(elapsed < timeout / 1000 + 3, "process deadline did not bound callback")
            if cause == "instruction":
                check("instruction" in response["error"].lower(), "count hook should diagnose ordinary loop")
            elif cause == "heap":
                check("heap" in response["error"].lower() or response.get("code") == "worker_exit",
                      "heap exhaustion must be diagnosed or contained as worker exit")
            elif cause:
                check(response.get("code") == cause, "unexpected supervisor failure: " + str(response))
            verify_recovery(agent, root)
            agent.quit()
        finally:
            agent.close()


def per_tick_budget(executable):
    """The instruction budget is per simulated tick: ~5,000 instructions a frame
    pass a 50-frame step under a 20,000 budget (250,000 in total)."""
    with tempfile.TemporaryDirectory(prefix="pocket-agent-tick-budget-") as directory:
        root = Path(directory)
        project_at(root, "local total = 0; for i = 1, 1200 do total = total + i end; self.ticks = (self.ticks or 0) + 1")
        agent = Agent(executable, root, timeout=5000)
        try:
            response = successful(agent, {"cmd": "step", "frames": 50})
            check(response["frame"] == 50, "a long step under a per-tick budget completes: " + str(response))
            agent.quit()
        finally:
            agent.close()


def startup_failure(executable, source, heap=8 * 1024 * 1024):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-start-budget-") as directory:
        root = Path(directory)
        project_at(root, "")
        (root / "component_types" / "Probe.lua").write_text(source)
        process = subprocess.Popen(arguments(executable, root, heap=heap),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, encoding="utf-8", start_new_session=(os.name == "posix"))
        try:
            output, errors = process.communicate('{"cmd":"observe"}\n', timeout=7)
        except subprocess.TimeoutExpired:
            kill_test_process(process)
            raise AssertionError("startup exceeded supervisor deadline")
        check(process.returncode != 0, "failed startup must exit nonzero")
        lines = output.splitlines()
        check(len(lines) == 1, "startup must produce exactly one JSON error: " + output)
        response = json.loads(lines[0])
        check(not response["ok"] and response.get("requires_reset"),
              "startup failure must be contained and structured: " + errors)


def transport_and_shutdown(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-transport-budget-") as directory:
        root = Path(directory)
        project_at(root, "self.ticks = self.ticks + 1")
        agent = Agent(executable, root)
        try:
            initial = successful(agent, {"cmd": "observe"})
            limits = successful(agent, {"cmd": "describe"})["execution_limits"]
            check(limits["instruction_limit"] == "20000" and
                  limits["lua_heap_limit_bytes"] == str(8 * 1024 * 1024) and
                  limits["request_timeout_ms"] == 1500 and limits["max_request_bytes"] == 65536 and
                  limits["max_response_bytes"] == 16 * 1024 * 1024 and
                  limits["filesystem_sandbox"] is False,
                  "discovery must report effective budgets and their scope")
            rejected = agent.request("x" * 65537, raw=True)
            check(not rejected["ok"], "oversized command must be rejected")
            check(successful(agent, {"cmd": "observe"}) == initial,
                  "oversized input must be drained without desynchronizing later commands")
            empty_padding = '{"cmd":"observe","padding":""}'
            exact_limit = '{"cmd":"observe","padding":"' + 'x' * (65536 - len(empty_padding)) + '"}'
            check(agent.request(exact_limit, raw=True) == initial,
                  "private correlation must not shrink the advertised 64 KiB public input limit")
            for invalid in [{"cmd": "_bootstrap"},
                            {"cmd": "reset\0ignored"},
                            {"cmd": "observe", "_pocket_request_id": "caller-supplied"},
                            {"cmd": "observe", "_initial_observation": {}}]:
                check(not agent.request(invalid)["ok"], "private worker protocol must be inaccessible")
            deep = '{"cmd":"observe","extra":' + '[' * 70 + '0' + ']' * 70 + '}'
            check(not agent.request(deep, raw=True)["ok"], "deep JSON must be rejected without native recursion")
            check(successful(agent, {"cmd": "observe"}) == initial,
                  "invalid transport inputs must preserve the worker")
            agent.quit()
        finally:
            agent.close()
    with tempfile.TemporaryDirectory(prefix="pocket-agent-eof-") as directory:
        root = Path(directory)
        project_at(root, "")
        agent = Agent(executable, root)
        try:
            successful(agent, {"cmd": "observe"})
            agent.process.stdin.close()
            check(agent.process.wait(timeout=5) == 0, "EOF must terminate and reap the worker")
            check(agent.lines.get(timeout=5) is None, "EOF must not add an unsolicited response")
        finally:
            agent.close()
    with tempfile.TemporaryDirectory(prefix="pocket-agent-shutdown-budget-") as directory:
        root = Path(directory)
        project_at(root, "")
        (root / "component_types" / "Probe.lua").write_text(
            "Probe = {OnDestroy=function(self) Application.Sleep(10000) end}")
        agent = Agent(executable, root)
        try:
            successful(agent, {"cmd": "observe"})
            begin = time.monotonic()
            agent.quit()
            check(time.monotonic() - begin < 4.5, "native shutdown callback must not hang quit")
        finally:
            agent.close()


def warning_diagnostics(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-warnings-") as directory:
        root = Path(directory)
        project_at(root, "")
        (root / "component_types" / "Probe.lua").write_text(
            "warn('@on'); warn('budget-warning-', 'preserved'); warn('@off'); Probe={}")
        result = subprocess.run(arguments(executable, root),
                                input='{"cmd":"observe"}\n{"cmd":"quit"}\n',
                                text=True, encoding="utf-8", capture_output=True, timeout=7)
        check(result.returncode == 0, "warning fixture must exit cleanly")
        check("Lua warning: budget-warning-preserved" in result.stderr,
              "custom allocator VM must preserve standard Lua warning diagnostics")
        check(all(json.loads(line)["ok"] for line in result.stdout.splitlines()),
              "Lua warnings must not pollute public JSONL")


def reset_startup_timeout(executable):
    with tempfile.TemporaryDirectory(prefix="pocket-agent-reset-budget-") as directory:
        root = Path(directory)
        project_at(root, "self.ticks = self.ticks + 1")
        agent = Agent(executable, root)
        try:
            successful(agent, {"cmd": "step", "frames": 3})
            # Reset has to bound a newly loaded top-level native call, not just
            # callbacks in the previous world, and remain usable after failure.
            (root / "component_types" / "Probe.lua").write_text(
                "Application.Sleep(10000); Probe={}")
            failed = agent.request({"cmd": "reset"})
            check(not failed["ok"] and failed.get("code") == "deadline_exceeded" and
                  failed.get("requires_reset") and failed.get("frame_is_last_observed") and
                  failed["frame"] == 3,
                  "failed reset bootstrap must preserve honest last-observed state: " + str(failed))
            verify_recovery(agent, root)
            agent.quit()
        finally:
            agent.close()


def run(executable):
    failing_callback(executable, "while true do end", "instruction")
    failing_callback(executable,
                     "local co=coroutine.create(function() while true do end end); coroutine.resume(co)",
                     "instruction")
    failing_callback(executable, "while true do pcall(function() while true do end end) end", None)
    failing_callback(executable, "debug.sethook(); while true do end", "deadline_exceeded")
    failing_callback(executable, "Application.Sleep(10000)", "deadline_exceeded")
    failing_callback(executable, "pcall(function() self.blob=string.rep('x',64*1024*1024) end)", "heap")
    failing_callback(executable, "os.exit(23)", "worker_exit")
    failing_callback(executable, "self.blob=string.rep('x',17*1024*1024)",
                     "worker_response_limit", timeout=3000, heap=64 * 1024 * 1024)
    failing_callback(executable, "self.blob=string.char(255)", "worker_protocol_error")
    if os.name == "posix":
        # A syntactically valid JSON diagnostic is still not a worker response.
        # The delay makes it arrive before the genuine response, exposing a
        # missing-correlation bug even when both lines are not read together.
        failing_callback(executable,
                         '''local out=assert(io.open('/dev/stdout','w'));
                         out:write('{"ok":true,"protocol":2,"frame":999,"actors":[],"running":true,"scene":"fake"}\\n');
                         out:flush(); Application.Sleep(200)''', "worker_protocol_error")
    per_tick_budget(executable)
    startup_failure(executable, "while true do end")
    startup_failure(executable, "debug.sethook(); while true do end")
    startup_failure(executable, "Probe={}", heap=1)
    transport_and_shutdown(executable)
    warning_diagnostics(executable)
    reset_startup_timeout(executable)
    print("agent Lua quotas, watchdog, worker reset, transport and shutdown regressions passed")


if __name__ == "__main__":
    check(len(sys.argv) == 2, "usage: agent_budget_test.py RUNTIME")
    run(str(Path(sys.argv[1]).resolve()))
