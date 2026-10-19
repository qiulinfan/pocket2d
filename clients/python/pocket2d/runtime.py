"""A JSONL client for the Pocket2D agent protocol."""
import itertools
import json
import queue
import subprocess
import threading
from pathlib import Path


class ProtocolError(RuntimeError):
    """The runtime answered with ok:false. The response is kept in .response."""

    def __init__(self, response):
        super().__init__(response.get("error", "agent request failed"))
        self.response = response


class Runtime:
    """One supervised runtime process (`game --headless --agent`).

    request() sends one command and returns its response; failures raise
    ProtocolError unless check=False. Use as a context manager or call close().
    """

    def __init__(self, executable, project, seed=0, save_dir=None, extra_args=(), timeout=60.0):
        command = [str(executable), "--headless", "--agent", "--project", str(Path(project)), "--seed", str(seed)]
        if save_dir is not None:
            command += ["--save-dir", str(save_dir)]
        command += list(extra_args)
        self.timeout = timeout
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self._lines = queue.Queue()
        self.stderr = []
        self._ids = itertools.count(1)
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self):
        for line in self.process.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def _read_stderr(self):
        for line in self.process.stderr:
            self.stderr.append(line)
            del self.stderr[:-200]

    def request(self, command, check=True):
        request_id = next(self._ids)
        self.process.stdin.write(json.dumps({**command, "id": request_id}) + "\n")
        self.process.stdin.flush()
        try:
            line = self._lines.get(timeout=self.timeout)
        except queue.Empty:
            raise TimeoutError("runtime did not respond: " + "".join(self.stderr[-20:])) from None
        if line is None:
            raise RuntimeError("runtime exited: " + "".join(self.stderr[-20:]))
        response = json.loads(line)
        if response.get("id") != request_id:
            raise RuntimeError("response correlation mismatch")
        if check and not response.get("ok"):
            raise ProtocolError(response)
        return response

    def call(self, name, args=None, *, actor=None, component=None, check=True, **options):
        """Run the game-declared command `name` (the call command).

        Every targeted Lua component defining OnAgentCommand(self, name, args)
        handles it; response["call"]["results"] holds their return values. A
        handler error raises ProtocolError (code agent_command_failed) unless
        check=False. Other options (select, events, game, ...) are passed on.
        """
        command = {"cmd": "call", "name": name, "args": {} if args is None else args, **options}
        if actor is not None:
            command["actor"] = str(actor)
        if component is not None:
            command["component"] = component
        return self.request(command, check=check)

    def close(self):
        if self.process.poll() is None:
            try:
                self.request({"cmd": "quit"}, check=False)
            except (RuntimeError, TimeoutError, BrokenPipeError, OSError):
                pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
