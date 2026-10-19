#!/usr/bin/env python3
"""LAN-only mode gateway. The balance service and its API key remain separate."""
import json
import math
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

HOST = os.environ.get("CONTROL_HOST", "0.0.0.0")
PORT = int(os.environ.get("CONTROL_PORT", "8082"))
STATE_FILE = Path(os.environ.get("CONTROL_STATE_FILE", str(Path.home() / ".local/state/espfish/control.json")))
PAGE = Path(__file__).with_name("index.html")


class State:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.config = {"mode": "balance", "swap_axes": False, "invert_x": False, "invert_y": False}
        self.device = None
        self.last_seen = 0.0
        self.calibration = 0
        try:
            self.config.update(self.validate(json.loads(path.read_text(encoding="utf-8"))))
        except (OSError, ValueError, TypeError):
            pass

    @staticmethod
    def validate(data):
        if not isinstance(data, dict) or not data or set(data) - {"mode", "swap_axes", "invert_x", "invert_y"}:
            raise ValueError("Unknown or missing settings")
        if "mode" in data and data["mode"] not in ("balance", "pet"):
            raise ValueError("Mode must be balance or pet")
        for key in ("swap_axes", "invert_x", "invert_y"):
            if key in data and type(data[key]) is not bool:
                raise ValueError("Axis settings must be boolean")
        return data

    def update(self, data):
        data = self.validate(data)
        with self.lock:
            config = self.config | data
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_suffix(".tmp")
            temporary.write_text(json.dumps(config) + "\n", encoding="utf-8")
            temporary.replace(self.path)
            self.config = config

    def report(self, data):
        if not isinstance(data, dict) or data.get("mode") not in ("balance", "pet"):
            raise ValueError("Invalid device report")
        device = {"mode": data["mode"], "imu": data.get("imu") is True}
        for key in ("heap", "x", "y", "ax", "ay", "render_bytes", "uptime", "frames"):
            value = data.get(key, 0)
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError("Invalid telemetry")
            device[key] = value
        with self.lock:
            self.device = device
            self.last_seen = time.monotonic()

    def calibrate(self):
        with self.lock:
            self.calibration += 1

    def snapshot(self):
        with self.lock:
            return {**self.config, "calibration": self.calibration, "device": self.device,
                    "online": self.last_seen > 0 and time.monotonic() - self.last_seen < 20}


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, value, content_type="application/json; charset=utf-8"):
        body = value if isinstance(value, bytes) else json.dumps(value, allow_nan=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/api/state":
            self.reply(200, self.server.state.snapshot())
        else:
            self.reply(404, {"error": "Not found"})

    def do_POST(self):
        origin = self.headers.get("Origin")
        if origin and urlsplit(origin).netloc != self.headers.get("Host"):
            self.reply(403, {"error": "Cross-origin request rejected"})
            return
        if self.headers.get_content_type() != "application/json":
            self.reply(415, {"error": "Expected application/json"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 2048:
                raise ValueError("Body must be 1..2048 bytes")
            data = json.loads(self.rfile.read(length))
            if self.path == "/api/mode":
                self.server.state.update(data)
            elif self.path == "/api/device":
                self.server.state.report(data)
            elif self.path == "/api/calibrate" and data == {}:
                self.server.state.calibrate()
            else:
                self.reply(404, {"error": "Not found"})
                return
        except (ValueError, TypeError, UnicodeError) as error:
            self.reply(400, {"error": str(error)})
            return
        except OSError:
            self.reply(503, {"error": "Unable to save settings"})
            return
        self.reply(200, self.server.state.snapshot())

    def log_message(self, format, *args):
        pass


def make_server(address=(HOST, PORT), state_file=STATE_FILE):
    server = ThreadingHTTPServer(address, Handler)
    server.daemon_threads = True
    server.state = State(Path(state_file))
    return server


if __name__ == "__main__":
    make_server().serve_forever()
