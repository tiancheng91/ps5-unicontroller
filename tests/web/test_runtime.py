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
        status, launcher = request("GET", "/api/v1/launcher")
        assert status == 200 and launcher["state"] == "unsupported"
        assert launcher["port"] == port and launcher["title_id"] == "UNIC00001"
        assert request("POST", "/api/v1/launcher", auth=False)[0] == 403
        assert request("POST", "/api/v1/launcher")[0] == 503
        assert request("DELETE", "/api/v1/launcher")[0] == 503
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
        # The host mock exposes four distinct signed-in users, matching backend
        # capacity without pretending that native physical adapters exist.
        status, users = request("GET", "/api/v1/users")
        assert status == 200
        assert [user["user_id"] for user in users["users"]] == [1, 2, 3, 4]
        assert users["preferred_user_id"] == 1
        assert request("GET", "/api/v1/usb")[1]["state"] == "off"
        assert request("GET", "/api/v1/bluetooth/diagnostics")[1] == {
            "schema_version": 1,
            "snapshot": True,
            "devices": [],
        }
        status, diagnostic = request("GET", "/api/v1/usb/diagnostics")
        assert status == 200 and diagnostic == {
            "schema_version": 1,
            "snapshot": True,
            "devices": [],
        }
        assert (
            request("POST", "/api/v1/usb/start", {"user_id": 1}, auth=False)[0] == 403
        )
        assert request("POST", "/api/v1/usb/start", {"user_id": 5})[0] == 409
        assert request("POST", "/api/v1/usb/start", {"user_id": 0})[0] == 400
        assert (
            request("POST", "/api/v1/usb/start", {"user_id": 1, "extra": True})[0]
            == 400
        )
        assert request("POST", "/api/v1/usb/start", {"user_id": 1})[0] == 202
        # Independent physical transports may start together. Host-native
        # adapters then fail explicitly; no synthetic device/input is invented.
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 2})[0] == 202
        assert request("POST", "/api/v1/test-controller", {"user_id": 1})[0] == 409
        time.sleep(0.15)
        assert request("GET", "/api/v1/usb")[1]["state"] == "failed"
        assert request("POST", "/api/v1/usb/stop")[0] == 200
        assert request("GET", "/api/v1/bluetooth")[1]["state"] == "failed"
        assert request("POST", "/api/v1/bluetooth/stop")[0] == 200
        assert request("GET", "/api/v1/bluetooth")[1]["running"] is False
        assert (
            request("POST", "/api/v1/bluetooth/start", {"user_id": 1}, auth=False)[0]
            == 403
        )
        assert request("POST", "/api/v1/bluetooth/scan", {"seconds": 60})[0] == 409
        assert request("POST", "/api/v1/bluetooth/pair", {"seconds": 60})[0] == 400
        assert request("POST", "/api/v1/bluetooth/pair", {"discovery_id": 7})[0] == 409
        assert request("POST", "/api/v1/bluetooth/pair", {"discovery_id": 0})[0] == 400
        assert request("POST", "/api/v1/bluetooth/scan/stop")[0] == 409
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 5})[0] == 409
        assert request("POST", "/api/v1/bluetooth/start", {"user_id": 1})[0] == 202
        assert request("POST", "/api/v1/test-controller", {"user_id": 1})[0] == 409
        time.sleep(0.15)
        assert request("GET", "/api/v1/bluetooth")[1]["state"] == "failed"
        assert request("POST", "/api/v1/bluetooth/stop")[0] == 200

        # Assignments require exactly the source-specific identity plus a
        # bounded slot and signed-in user. No route opens a supplied pathname.
        for source, identity, value in (
            ("bluetooth", "address", "01:02:03:04:05:06"),
            ("usb", "node", "/dev/ugen1.2"),
        ):
            endpoint = f"/api/v1/{source}/assign"
            assignment = {"slot": 0, identity: value, "user_id": 4}
            assert request("POST", endpoint, assignment, auth=False)[0] == 403
            for malformed in (
                {},
                dict(assignment, slot=-1),
                dict(assignment, slot=4),
                dict(assignment, slot=True),
                dict(assignment, user_id=0),
                dict(assignment, user_id="4"),
                dict(assignment, extra=True),
                {"slot": 0, "user_id": 4},
                dict(assignment, **{identity: 123}),
                dict(assignment, **{identity: ""}),
                dict(assignment, **{identity: "x" * 128}),
            ):
                assert request("POST", endpoint, malformed)[0] == 400, malformed
            assert request("POST", endpoint, dict(assignment, user_id=5))[0] == 409
            assert (
                request(
                    "POST", endpoint, dict(assignment, **{identity: "bad\\identity"})
                )[0]
                == 409
            )
            # A well-formed assignment still cannot bind an absent/stale device.
            assert request("POST", endpoint, assignment)[0] == 409

        for page in ["/", "/controller", "/settings"]:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            connection.request("GET", page)
            result = connection.getresponse()
            assert result.status == 200 and result.read().startswith(b"<!doctype html>")
            connection.close()
        for removed in [
            "/assets/control/control.js",
            "/assets/controller/controller.js",
            "/assets/shared/ui.js",
        ]:
            assert request("GET", removed)[0] == 404
        # Production embeds binary icons and pinned scripts intact; demo data stays outside.
        for resource, local_path, mime in [
            ("/assets/icon.png", "web/icon.png", "image/png"),
            ("/assets/brand.svg", "web/brand.svg", "image/svg+xml"),
            (
                "/assets/vendor/alpine-csp-3.14.9.min.js",
                "web/vendor/alpine-csp-3.14.9.min.js",
                "application/javascript",
            ),
        ]:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            c.request("GET", resource)
            response = c.getresponse()
            assert response.status == 200 and mime in response.getheader("Content-Type")
            assert response.read() == Path(local_path).read_bytes()
            c.close()
        assert request("GET", "/__preview__.js")[0] == 404
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
        assert settings["bluetooth_auto_start"] is False
        assert settings["bluetooth_auto_user"] == 0
        assert settings["bluetooth_auto_trigger"] == "login"
        resume = dict(
            settings, bluetooth_auto_start=True, bluetooth_auto_trigger="resume"
        )
        assert request("POST", "/api/v1/settings", resume)[1] == resume
        assert json.loads(path.read_text()) == resume
        for bad_trigger in ["awake", True, 2, None]:
            assert (
                request(
                    "POST",
                    "/api/v1/settings",
                    dict(settings, bluetooth_auto_trigger=bad_trigger),
                )[0]
                == 400
            )

        legacy = {
            k: v for k, v in settings.items() if not k.startswith("bluetooth_auto_")
        }
        assert request("POST", "/api/v1/settings", legacy)[1] == settings
        for invalid_mode in ["login", 1, None]:
            assert (
                request(
                    "POST",
                    "/api/v1/settings",
                    dict(settings, bluetooth_auto_start=invalid_mode),
                )[0]
                == 400
            )
        # Enabled with no remembered account remains inert; never invent a target.
        enabled = dict(settings, bluetooth_auto_start=True)
        assert request("POST", "/api/v1/settings", enabled)[1] == enabled
        assert json.loads(path.read_text()) == enabled
        # HTTP clients cannot replace the server-owned remembered account.
        assert (
            request("POST", "/api/v1/settings", dict(settings, bluetooth_auto_user=2))[
                1
            ]
            == settings
        )
        changed = dict(settings, swap_ab=True, recording_level="debug")
        assert request("POST", "/api/v1/settings", changed)[0] == 200
        assert json.loads(path.read_text()) == changed
        invalid = dict(changed, pairing_secret="must-not-be-accepted")
        assert request("POST", "/api/v1/settings", invalid)[0] == 400
        assert request("GET", "/api/v1/settings")[1] == changed
        status, state = request("POST", "/api/v1/test-controller", {"user_id": 1})
        assert status == 200 and state["enabled"]
        assert request("POST", "/api/v1/settings", settings)[0] == 409
        # Stopping an unrelated physical transport must not tear down the
        # manually enabled test-controller session or accept a stale assignment.
        assert request("POST", "/api/v1/bluetooth/stop")[0] == 200
        assert request("POST", "/api/v1/usb/stop")[0] == 200
        assert (
            request("GET", "/api/v1/test-controller")[1]["session_id"]
            == state["session_id"]
        )
        assert (
            request(
                "POST",
                "/api/v1/usb/assign",
                {"slot": 0, "node": "/dev/ugen1.2", "user_id": 2},
            )[0]
            == 409
        )
        assert (
            request("GET", "/api/v1/test-controller")[1]["session_id"]
            == state["session_id"]
        )
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
        previous_session = state["session_id"]
        for user_id in (2, 3, 4):
            status, next_state = request(
                "POST", "/api/v1/test-controller", {"user_id": user_id}
            )
            assert status == 200 and next_state["user_id"] == user_id
            assert next_state["session_id"] != previous_session
            stale_frame = dict(
                frame, session_id=previous_session, sequence=99, axes=[0] * 4
            )
            assert (
                request("POST", "/api/v1/test-controller/input", stale_frame)[0] == 409
            )
            assert request("POST", "/api/v1/bluetooth/start", {"user_id": 1})[0] == 409
            assert request("POST", "/api/v1/usb/start", {"user_id": 1})[0] == 409
            assert request("DELETE", "/api/v1/test-controller")[0] == 200
            previous_session = next_state["session_id"]
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
