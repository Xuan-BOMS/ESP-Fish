import json
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import control_service


class ControlServiceTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "control.json"
        self.server = control_service.make_server(("127.0.0.1", 0), self.path)
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.base = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.directory.cleanup()

    def request(self, path, data=None, origin=None):
        headers = {"Content-Type": "application/json"}
        if origin:
            headers["Origin"] = origin
        request = Request(self.base + path, data=None if data is None else json.dumps(data).encode(), headers=headers)
        with urlopen(request, timeout=2) as response:
            return json.load(response)

    def test_mode_is_persisted_and_only_confirmed_after_device_report(self):
        state = self.request("/api/mode", {"mode": "pet", "invert_x": True})
        self.assertFalse(state["online"])
        self.assertIsNone(state["device"])
        restored = control_service.State(self.path).snapshot()
        self.assertEqual(restored["mode"], "pet")
        self.assertTrue(restored["invert_x"])
        report = self.request("/api/device", {"mode": "balance", "heap": 123456, "imu": True})
        self.assertEqual(report["mode"], "pet")
        self.assertEqual(report["device"]["mode"], "balance")
        self.assertTrue(report["online"])
        report = self.request("/api/device", {"mode": "pet", "heap": 105024})
        self.assertEqual(report["device"]["mode"], "pet")
        self.server.state.last_seen -= 30
        self.assertFalse(self.request("/api/state")["online"])

    def test_invalid_commands_do_not_change_saved_mode(self):
        for data in ({"mode": "invalid"}, {"invert_x": "false"}, {"unknown": True}, [], {}):
            with self.subTest(data=data), self.assertRaises(HTTPError) as error:
                self.request("/api/mode", data)
            self.assertEqual(error.exception.code, 400)
        self.assertEqual(self.request("/api/state")["mode"], "balance")
        self.assertFalse(self.path.exists())

    def test_other_websites_cannot_change_mode(self):
        with self.assertRaises(HTTPError) as error:
            self.request("/api/mode", {"mode": "pet"}, origin="https://example.com")
        self.assertEqual(error.exception.code, 403)

    def test_bad_device_telemetry_is_rejected(self):
        with self.assertRaises(HTTPError) as error:
            self.request("/api/device", {"mode": "pet", "ax": float("nan")})
        self.assertEqual(error.exception.code, 400)

    def test_page_is_served_as_utf8(self):
        with urlopen(self.base, timeout=2) as response:
            self.assertIn("charset=utf-8", response.headers["Content-Type"])
            self.assertIn("ESP Fish", response.read().decode("utf-8"))

    def test_calibration_reaches_device_without_changing_saved_mode(self):
        self.request("/api/mode", {"mode": "pet", "invert_y": True})
        saved = self.path.read_bytes()
        first = self.request("/api/calibrate", {})
        second = self.request("/api/calibrate", {})
        self.assertEqual(second["calibration"], first["calibration"] + 1)
        report = self.request("/api/device", {"mode": "pet"})
        self.assertEqual(report["calibration"], second["calibration"])
        self.assertEqual(self.path.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()
