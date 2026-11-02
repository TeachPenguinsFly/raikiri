from __future__ import annotations

import hashlib
import hmac
import json
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import urlsplit

from websockets.sync.server import ServerConnection, serve

KEY = "fake-key"
SECRET = "fake-secret-that-is-long-enough-for-the-server"


def expected_signature(stamp: str, method: str, path: str, query: str, body: str) -> str:
    message = "\n".join((stamp, method, path, query, body))
    return hmac.new(SECRET.encode(), message.encode(), hashlib.sha256).hexdigest()


@dataclass
class Seen:
    method: str
    path: str
    query: str
    body: str
    headers: dict[str, str]


Reply = tuple[int, Any] | tuple[int, Any, dict[str, str]]
Handler = Callable[[Seen], Reply]


@dataclass
class FakeServer:
    """REST and the stream, with the signature check apid does and scripted answers."""

    routes: dict[str, list[Handler] | Handler] = field(default_factory=dict)
    seen: list[Seen] = field(default_factory=list)
    frames: list[list[Any]] = field(default_factory=list)  # per connection; "drop" ends it
    received: list[list[dict[str, Any]]] = field(default_factory=list)
    upgrade_status: int = 101
    clock_offset_ms: int = 0
    hello_epoch: str = "e1"
    heartbeat_ms: int = 200
    skew_upgrades: int = 0

    def start(self) -> None:
        owner = self

        class Http(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_: Any) -> None:
                pass

            def serve_any(self) -> None:
                parts = urlsplit(self.path)
                body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
                seen = Seen(self.command, parts.path, parts.query, body, dict(self.headers.items()))
                owner.seen.append(seen)
                status, payload, extra = owner.answer(seen)
                data = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/problem+json" if status >= 400 else "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("X-Request-Id", "req-1")
                for name, value in extra.items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(data)

            do_GET = do_POST = do_DELETE = serve_any

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), Http)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        self.ws = serve(self.ws_handler, "127.0.0.1", 0, process_request=self.check_upgrade)
        threading.Thread(target=self.ws.serve_forever, daemon=True).start()

    def stop(self) -> None:
        self.http.shutdown()
        self.ws.shutdown()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.http.server_address[1]}"

    @property
    def ws_url(self) -> str:
        return f"http://127.0.0.1:{self.ws.socket.getsockname()[1]}"

    def check_signature(self, seen: Seen) -> bool:
        stamp = seen.headers.get("X-Raikiri-Timestamp", "")
        if seen.headers.get("X-Raikiri-Key") != KEY:
            return False
        want = expected_signature(stamp, seen.method, seen.path, seen.query, seen.body)
        return seen.headers.get("X-Raikiri-Signature") == want

    def answer(self, seen: Seen) -> tuple[int, Any, dict[str, str]]:
        tail = seen.path.removeprefix("/api/v1")
        if tail == "/time":
            return 200, {"server_time_ms": str(int(time.time() * 1000) + self.clock_offset_ms)}, {}
        if not self.check_signature(seen):
            return 401, {"status": 401, "code": "auth-failed", "title": "bad signature"}, {}
        route = self.routes.get(f"{seen.method} {tail}")
        if route is None:
            return 404, {"status": 404, "code": "not-found", "title": "no route"}, {}
        handler = route if callable(route) else (route.pop(0) if len(route) > 1 else route[0])
        reply = handler(seen)
        return (reply[0], reply[1], reply[2] if len(reply) > 2 else {})

    def check_upgrade(self, connection: ServerConnection, request: Any) -> Any:
        if self.upgrade_status != 101:
            body = json.dumps({"status": self.upgrade_status, "code": "auth-failed", "title": "no"})
            return connection.respond(self.upgrade_status, body)
        stamp = request.headers.get("X-Raikiri-Timestamp", "")
        if self.skew_upgrades > 0:
            self.skew_upgrades -= 1
            return connection.respond(401, json.dumps({"status": 401, "code": "timestamp-skew", "title": "skew"}))
        want = expected_signature(stamp, "GET", "/api/v1/stream", "", "")
        if request.headers.get("X-Raikiri-Signature") != want:
            return connection.respond(401, json.dumps({"status": 401, "code": "auth-failed", "title": "no"}))
        return None

    def ws_handler(self, connection: ServerConnection) -> None:
        index = len(self.received)
        inbox: list[dict[str, Any]] = []
        self.received.append(inbox)
        connection.send(
            json.dumps(
                {
                    "type": "hello",
                    "epoch": self.hello_epoch,
                    "seq": "0",
                    "heartbeat_ms": self.heartbeat_ms,
                    "client_id": "c",
                }
            )
        )
        script = self.frames[index] if index < len(self.frames) else []
        try:
            # the client's subscribes arrive right after the hello
            first = connection.recv(timeout=2)
            inbox.append(json.loads(first))
            for frame in script:
                if frame == "drop":
                    return
                if frame == "silence":
                    time.sleep(5)
                    continue
                connection.send(json.dumps(frame))
            while True:
                inbox.append(json.loads(connection.recv(timeout=5)))
        except Exception:  # noqa: BLE001 - the test ended, or the client went away
            pass
