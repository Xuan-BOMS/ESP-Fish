#!/usr/bin/env python3
"""LAN-only mode gateway. The balance service and its API key remain separate."""
import json
import hashlib
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
LIBRARY = Path(os.environ.get("CONTROL_LIBRARY", str(Path(__file__).with_name("library"))))
CACHE_LIMIT = 1750000
MAX_CLIPS = 16


class State:
    def __init__(self, path, library=LIBRARY):
        self.path = path
        self.lock = threading.Lock()
        self.config = {"mode": "balance", "swap_axes": False, "invert_x": False, "invert_y": False}
        self.device = None
        self.last_seen = 0.0
        self.calibration = 0
        self.library = library
        try:
            catalog = json.loads((library / "catalog.json").read_text(encoding="utf-8"))
        except FileNotFoundError:
            catalog = {"clips": [], "defaults": []}
        self.clips = {clip["id"]: clip for clip in catalog["clips"]}
        self.defaults = catalog["defaults"]
        try:
            saved = json.loads(path.read_text(encoding="utf-8"))
            animations = saved.pop("animations", self.defaults)
            self.config.update(self.validate(saved))
            self.validate_playlist(animations)
            self.config["animations"] = animations
        except (OSError, ValueError, TypeError):
            pass
        self.config.setdefault("animations", self.defaults)

    def validate_playlist(self, ids):
        if not isinstance(ids, list) or not 1 <= len(ids) <= MAX_CLIPS or any(not isinstance(i, str) for i in ids):
            raise ValueError(f"Select 1..{MAX_CLIPS} animations")
        if len(set(ids)) != len(ids) or any(i not in self.clips for i in ids):
            raise ValueError("Unknown or duplicate animation")
        if sum(self.clips[i]["bytes"] for i in ids) > CACHE_LIMIT:
            raise ValueError("Selection exceeds ESP32 cache capacity")

    def playlist(self):
        clips = [{k: self.clips[i][k] for k in ("id", "role", "bytes", "sha256")}
                 for i in self.config["animations"] if i in self.clips]
        revision = hashlib.sha256(json.dumps(clips, sort_keys=True).encode()).hexdigest()[:16]
        return {"revision": revision, "clips": clips}

    def select(self, data):
        if not isinstance(data, dict) or set(data) != {"animations"}:
            raise ValueError("Expected animations")
        self.validate_playlist(data["animations"])
        self._save(data)

    def catalog(self):
        with self.lock:
            return {"clips": list(self.clips.values()), "selected": self.config["animations"],
                    "capacity": CACHE_LIMIT, "max_clips": MAX_CLIPS}

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
        self._save(data)

    def _save(self, data):
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
        for key in ("heap", "x", "y", "ax", "ay", "render_bytes", "uptime", "frames", "cache_bytes", "cache_capacity", "downloaded", "download_total", "cached_clips"):
            value = data.get(key, 0)
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError("Invalid telemetry")
            device[key] = value
        for key in ("library_revision", "sync_status", "sync_error"):
            value = data.get(key, "")
            if not isinstance(value, str) or len(value) > 160:
                raise ValueError("Invalid cache status")
            device[key] = value
        with self.lock:
            self.device = device
            self.last_seen = time.monotonic()

    def calibrate(self):
        with self.lock:
            self.calibration += 1

    def snapshot(self):
        with self.lock:
            return {**self.config, "playlist": self.playlist(), "calibration": self.calibration, "device": self.device,
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
        elif self.path == "/api/library":
            self.reply(200, self.server.state.catalog())
        elif self.path.startswith(("/assets/", "/media/", "/thumbs/")):
            prefix, filename = self.path.strip("/").split("/", 1)
            identifier, separator, extension = filename.partition(".")
            expected = {"assets": ("pak", "application/octet-stream"), "media": ("webm", "video/webm"),
                        "thumbs": ("jpg", "image/jpeg")}[prefix]
            if not separator or extension != expected[0] or identifier not in self.server.state.clips:
                self.reply(404, {"error": "Not found"})
                return
            path = self.server.state.library / filename
            try:
                with path.open("rb") as source:
                    self.send_response(200)
                    self.send_header("Content-Type", expected[1])
                    self.send_header("Content-Length", str(path.stat().st_size))
                    self.send_header("Cache-Control", "public, max-age=3600")
                    self.send_header("X-Content-Type-Options", "nosniff")
                    self.end_headers()
                    while block := source.read(32768):
                        self.wfile.write(block)
            except FileNotFoundError:
                self.reply(404, {"error": "Media unavailable"})
            except (BrokenPipeError, ConnectionResetError):
                pass
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
            elif self.path == "/api/playlist":
                self.server.state.select(data)
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


def make_server(address=(HOST, PORT), state_file=STATE_FILE, library=LIBRARY):
    server = ThreadingHTTPServer(address, Handler)
    server.daemon_threads = True
    server.state = State(Path(state_file), Path(library))
    return server


if __name__ == "__main__":
    make_server().serve_forever()
