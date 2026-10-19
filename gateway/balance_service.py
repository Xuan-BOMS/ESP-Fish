#!/usr/bin/env python3
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


API_KEY = os.environ["DEEPSEEK_API_KEY"]
HOST = os.environ.get("BALANCE_HOST", "0.0.0.0")
PORT = int(os.environ.get("BALANCE_PORT", "8081"))
cache_lock = threading.Lock()
cached = None
cached_at = 0.0


def refresh_balance():
    global cached, cached_at
    request = Request(
        "https://api.deepseek.com/user/balance",
        headers={"Authorization": f"Bearer {API_KEY}", "Accept": "application/json"},
    )
    with urlopen(request, timeout=8) as response:
        body = json.load(response)
    if body.get("is_available") is not True or not isinstance(body.get("balance_infos"), list):
        raise ValueError("DeepSeek balance response is unavailable")
    balances = []
    for item in body["balance_infos"]:
        currency = item.get("currency")
        total = item.get("total_balance")
        if isinstance(currency, str) and isinstance(total, (str, int, float)):
            balances.append({"currency": currency, "total_balance": str(total)})
    if not balances:
        raise ValueError("DeepSeek returned no balance entries")
    with cache_lock:
        cached = balances
        cached_at = time.time()
    return balances, False


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        global cached, cached_at
        if self.path != "/balance":
            self.send_error(404)
            return
        try:
            balances, stale = refresh_balance()
        except (HTTPError, URLError, TimeoutError, ValueError, json.JSONDecodeError):
            with cache_lock:
                balances = cached
            if balances is None:
                self.send_error(503, "Balance temporarily unavailable")
                return
            stale = True
        payload = json.dumps({"balances": balances, "stale": stale}, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


def main():
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
