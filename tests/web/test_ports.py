"""Validate primary, fallback and exhausted listeners without port races."""

import http.client, json, os, socket, subprocess, sys, tempfile, time

binary = os.path.abspath(sys.argv[1])


def hold(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen()
    return s


def launch(path):
    return subprocess.Popen(
        [binary, "7456", path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def ready(p, port):
    for _ in range(100):
        if p.poll() is not None:
            raise AssertionError(p.stderr.read())
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=0.2)
            c.request("GET", "/api/v1/system")
            r = c.getresponse()
            r.read()
            c.close()
            if r.status == 200:
                return
        except OSError:
            pass
        time.sleep(0.02)
    raise AssertionError("listener not ready")


def stop(p):
    p.terminate()
    p.wait(timeout=3)


with tempfile.TemporaryDirectory() as d:
    p = launch(d + "/settings.json")
    try:
        ready(p, 7456)
    finally:
        stop(p)
    with hold(7456):
        p = launch(d + "/settings.json")
        try:
            ready(p, 7457)
        finally:
            stop(p)
        logs = p.stderr.read()
        assert "bind failed port=7456" in logs and ":7457" in logs
        with hold(7457):
            p = launch(d + "/settings.json")
            p.wait(timeout=3)
            assert p.returncode != 0
            logs = p.stderr.read()
            assert "bind failed port=7457" in logs
print("Primary 7456, fallback 7457, both occupied: PASS")
