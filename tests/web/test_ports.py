"""Exercise startup failure, retained resume listener and bounded fault recovery."""

import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

binary, fault_binary = map(os.path.abspath, sys.argv[1:3])


def reserve():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    s.listen()
    return s


def system(port):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=0.2)
    try:
        c.request("GET", "/api/v1/session")
        r = c.getresponse()
        assert r.status == 200
        return json.loads(r.read())
    finally:
        c.close()


def ready(p, port):
    for _ in range(200):
        if p.poll() is not None:
            raise AssertionError("HTTP process exited prematurely")
        try:
            return system(port)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("listener not ready")


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    with reserve() as occupied:
        port = occupied.getsockname()[1]
        p = subprocess.run(
            [binary, str(port), str(root / "blocked-settings.json")],
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert p.returncode != 0
        startup_log = (root / "blocked-settings.json.runtime.log").read_text()
        assert "stage=bind" in startup_log and "startup failed; exiting" in startup_log

    broken, fail_bind, rest = root / "break", root / "bind", root / "rest"
    env = dict(
        os.environ,
        UC_TEST_BREAK_LISTENER=str(broken),
        UC_TEST_FAIL_BIND=str(fail_bind),
        UC_TEST_REST=str(rest),
    )
    with reserve() as available:
        port = available.getsockname()[1]
    with (root / "settings.json.runtime.log").open("w+") as log:
        p = subprocess.Popen(
            [fault_binary, str(port), str(root / "settings.json")],
            stdout=subprocess.DEVNULL,
            stderr=log,
            env=env,
        )
        try:
            initial = ready(p, port)
            # Resume keeps the configured port and authentication token.
            for _ in range(3):
                p.send_signal(signal.SIGCONT)
                time.sleep(0.05)
                assert ready(p, port)["token"] == initial["token"]
            broken.touch()
            time.sleep(0.1)
            assert ready(p, port)["token"] == initial["token"]
            # Rest itself retains the healthy listener and the same token.
            rest.touch()
            time.sleep(0.05)
            assert ready(p, port)["token"] == initial["token"]
            fail_bind.touch()
            broken.touch()
            time.sleep(1.2)
            assert p.poll() is None
            log.flush()
            assert (
                "listener failed stage=bind"
                not in (root / "settings.json.runtime.log").read_text()
            )
            rest.unlink()
            p.wait(timeout=15)
            assert p.returncode != 0
            log.seek(0)
            logs = log.read()
            assert "retained listener healthy" in logs
            assert "listener recovered" in logs and "accept failed" in logs
            assert logs.count("listener failed stage=bind") == 10
            assert "recovery exhausted; exiting" in logs
        finally:
            if p.poll() is None:
                p.terminate()
                p.wait(timeout=3)
print("HTTP: startup failure, resume retention, recovery and bounded failure exit PASS")
