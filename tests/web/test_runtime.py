"""Host end-to-end smoke test: python3 test_runtime.py /path/to/host-binary."""

import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1])
with socket.socket() as reserve:
    reserve.bind(("127.0.0.1", 0))
    port = reserve.getsockname()[1]
origin = f"http://127.0.0.1:{port}"


def request(method, path, body=None, auth=True, origin_header=origin):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    headers = {}
    if auth:
        headers["X-UC-Token"] = token
    if origin_header:
        headers["Origin"] = origin_header
    payload = json.dumps(body) if body is not None else None
    connection.request(method, path, payload, headers)
    response = connection.getresponse()
    status, data = response.status, response.read()
    connection.close()
    return status, json.loads(data)


with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "settings.json"
    process = subprocess.Popen(
        [binary, str(port), str(path)], stdout=subprocess.DEVNULL
    )
    try:
        token = ""
        for attempt in range(100):
            try:
                status, session = request("GET", "/api/v1/session", auth=False)
                break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("runtime did not start")
        assert status == 200
        token = session["token"]
        # Embedded CSS/JS must match source bytes and have strict MIME types.
        root = Path(__file__).resolve().parents[2]
        for asset in (root / "web").rglob("*"):
            if asset.suffix not in (".css", ".js"):
                continue
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            url = "/assets/" + asset.relative_to(root / "web").as_posix()
            connection.request("GET", url)
            response = connection.getresponse()
            assert response.status == 200, url
            expected = (
                "text/css" if asset.suffix == ".css" else "application/javascript"
            )
            assert response.getheader("Content-Type").startswith(expected), url
            assert response.read() == asset.read_bytes(), url
            connection.close()
        assert len(token) == 64
        assert request("GET", "/api/v1/usb")[1]["state"] == "off"
        assert (
            request("POST", "/api/v1/usb/start", {"user_id": 1}, auth=False)[0] == 403
        )
        assert request("POST", "/api/v1/usb/start", {"user_id": 2})[0] == 409
        assert request("POST", "/api/v1/usb/start", {"user_id": 0})[0] == 400
        assert (
            request("POST", "/api/v1/usb/start", {"user_id": 1, "extra": True})[0]
            == 400
        )
        assert request("POST", "/api/v1/usb/start", {"user_id": 1})[0] == 202
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 1})[0] == 409
        assert request("POST", "/api/v1/test-controller", {"user_id": 1})[0] == 409
        time.sleep(0.15)
        assert request("GET", "/api/v1/usb")[1]["state"] == "failed"
        assert request("POST", "/api/v1/usb/stop")[0] == 200
        assert request("GET", "/api/v1/bluetooth")[1]["state"] == "off"
        assert (
            request("POST", "/api/v1/bluetooth/start", {"user_id": 1}, auth=False)[0]
            == 403
        )
        assert request("POST", "/api/v1/bluetooth/pair", {"seconds": 60})[0] == 409
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 2})[0] == 409
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 1})[0] == 202
        assert request("POST", "/api/v1/test-controller", {"user_id": 1})[0] == 409
        time.sleep(0.15)
        assert request("GET", "/api/v1/bluetooth")[1]["state"] == "failed"
        assert request("POST", "/api/v1/bluetooth/stop")[0] == 200

        for page in ["/", "/controller", "/settings"]:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            connection.request("GET", page)
            result = connection.getresponse()
            assert result.status == 200 and result.read().startswith(b"<!doctype html>")
            connection.close()
        assert (
            request("POST", "/api/v1/test-controller", {"user_id": 1}, auth=False)[0]
            == 403
        )
        assert (
            request(
                "POST", "/api/v1/test-controller", {"user_id": 1}, origin_header=None
            )[0]
            == 403
        )
        assert (
            request(
                "POST",
                "/api/v1/test-controller",
                {"user_id": 1},
                origin_header="http://evil.invalid",
            )[0]
            == 403
        )
        status, settings = request("GET", "/api/v1/settings")
        assert status == 200 and len(settings["button_mapping"]) == 32
        changed = dict(settings, swap_ab=True, recording_level="debug")
        assert request("POST", "/api/v1/settings", changed)[0] == 200
        assert json.loads(path.read_text()) == changed
        invalid = dict(changed, pairing_secret="must-not-be-accepted")
        assert request("POST", "/api/v1/settings", invalid)[0] == 400
        assert request("GET", "/api/v1/settings")[1] == changed
        status, state = request("POST", "/api/v1/test-controller", {"user_id": 1})
        assert status == 200 and state["enabled"]
        assert request("POST", "/api/v1/settings", settings)[0] == 409
        assert request("POST", "/api/v1/usb/start", {"user_id": 1})[0] == 409
        frame = dict(
            session_id=state["session_id"],
            sequence=1,
            buttons=1,
            axes=[0] * 4,
            triggers=[0] * 2,
        )
        assert request("POST", "/api/v1/test-controller/input", frame)[0] == 200
        assert request("POST", "/api/v1/test-controller/input", frame)[0] == 409
        frame["sequence"] = 2
        frame["axes"] = [999999] * 4
        assert request("POST", "/api/v1/test-controller/input", frame)[0] == 400
        assert request("DELETE", "/api/v1/test-controller")[0] == 200
        assert request("GET", "/api/v1/logs?level=trace")[0] == 200
        assert request("GET", "/api/v1/logs?level=unknown")[0] == 400
        assert request("POST", "/api/v1/settings", {"padding": "x" * 9000})[0] == 413
        # A slow partial request must not prevent independent requests or ticking.
        with socket.create_connection(("127.0.0.1", port)) as slow:
            slow.sendall(b"GET / HTTP/1.1\r\n")
            assert request("GET", "/api/v1/system")[0] == 200
        # Persist failures leave the settings snapshot intact.
        path.unlink()
        path.mkdir()
        assert request("POST", "/api/v1/settings", settings)[0] == 503
        assert request("GET", "/api/v1/settings")[1] == changed
        assert request("POST", "/api/v1/stop", auth=False)[0] == 403
        assert request("POST", "/api/v1/stop")[0] == 200
        process.wait(timeout=3)
        assert process.returncode == 0
        print("HTTP runtime integration checks passed")
    finally:
        process.terminate()
        process.wait(timeout=3)
