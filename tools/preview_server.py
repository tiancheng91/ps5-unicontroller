"""Live offline UI preview with isolated, browser-memory fixture data."""

import argparse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parent.parent
PAGES = {
    "/": "web/index.html",
    "/settings": "web/index.html",
    "/settings/": "web/index.html",
    "/controller": "web/controller.html",
    "/controller/": "web/controller.html",
}


class PreviewHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        self.serve_page()

    def do_HEAD(self):
        self.serve_page(head_only=True)

    def serve_page(self, head_only=False):
        path = urlsplit(self.path).path
        if path in PAGES:
            source = (ROOT / PAGES[path]).read_text()
            content = source.replace(
                "<head>", '<head><script src="/__preview__.js"></script>', 1
            ).encode()
            mime = "text/html; charset=utf-8"
        elif path == "/__preview__.js":
            content = (ROOT / "tools/preview/data.js").read_bytes()
            mime = "application/javascript; charset=utf-8"
        elif path.startswith("/assets/"):
            relative = Path(path.removeprefix("/assets/"))
            candidate = (ROOT / "web" / relative).resolve()
            if not candidate.is_relative_to(ROOT / "web") or not candidate.is_file():
                self.send_error(404)
                return
            content = candidate.read_bytes()
            mime = self.guess_type(str(candidate))
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(content)))
        self.send_header("Cache-Control", "no-store")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; frame-ancestors 'none'",
        )
        self.end_headers()
        if not head_only:
            self.wfile.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=7456)
    parser.add_argument("--bind", default="127.0.0.1")
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.bind, args.port), PreviewHandler)
    print(
        f"Demo preview: http://{args.bind}:{server.server_port}/ (no real hardware)",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
