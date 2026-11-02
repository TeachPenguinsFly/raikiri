from __future__ import annotations

import hashlib
import hmac
import json
import logging
import os
import random
import threading
import uuid
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from typing import Any, Generic, TypeVar
from urllib.parse import quote, urlencode

import httpx

from .errors import APIError, RaikiriError, error_from_problem

logger = logging.getLogger("raikiri")

PAPER_URL = "http://127.0.0.1:8900"
API_PREFIX = "/api/v1"
KEY_HEADER = "X-Raikiri-Key"
TIMESTAMP_HEADER = "X-Raikiri-Timestamp"
SIGNATURE_HEADER = "X-Raikiri-Signature"
RETRIABLE_STATUS = {429, 502, 503}

T = TypeVar("T")


def sign(secret: str, timestamp_ms: str, method: str, path: str, query: str, body: str) -> str:
    """Lowercase hex HMAC-SHA256 of ``timestamp\\nMETHOD\\npath\\nquery\\nbody``."""
    message = "\n".join((timestamp_ms, method, path, query, body))
    return hmac.new(secret.encode(), message.encode(), hashlib.sha256).hexdigest()


def new_idempotency_key() -> str:
    return uuid.uuid4().hex


@dataclass(frozen=True)
class RequestInfo:
    """What a logging hook sees of a request. No secret and no signature."""

    method: str
    url: str
    attempt: int
    idempotency_key: str | None


@dataclass(frozen=True)
class ResponseInfo:
    method: str
    url: str
    attempt: int
    status: int
    request_id: str | None
    elapsed_s: float
    replayed: bool
    rate_limit_remaining: int | None


@dataclass
class Hooks:
    on_request: Callable[[RequestInfo], None] | None = None
    on_response: Callable[[ResponseInfo], None] | None = None


@dataclass(frozen=True)
class Settings:
    url: str
    key: str
    secret: str = field(repr=False)
    timeout: float = 10.0
    retries: int = 3
    hooks: Hooks = field(default_factory=Hooks)


def settings_from_env(
    url: str | None, key: str | None, secret: str | None, timeout: float, retries: int, hooks: Hooks | None
) -> Settings:
    resolved_key = key if key is not None else os.environ.get("RAIKIRI_API_KEY")
    resolved_secret = secret if secret is not None else os.environ.get("RAIKIRI_API_SECRET")
    if not resolved_key or not resolved_secret:
        raise RaikiriError("no key: pass key= and secret=, or set RAIKIRI_API_KEY and RAIKIRI_API_SECRET")
    resolved_url = url or os.environ.get("RAIKIRI_API_URL") or PAPER_URL
    return Settings(resolved_url.rstrip("/"), resolved_key, resolved_secret, timeout, retries, hooks or Hooks())


@dataclass
class Call(Generic[T]):
    """One API call, independent of how it is sent. Both clients run the same ones."""

    method: str
    path: str
    parse: Callable[[Any], T]
    query: Mapping[str, Any] | None = None
    body: Any = None
    idempotency_key: str | None = None
    signed: bool = True

    @property
    def mutating(self) -> bool:
        return self.method != "GET"


class Signer:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.clock_offset_ms = 0
        self._last_ms = 0
        self._lock = threading.Lock()

    def now_ms(self, wall_ms: int) -> str:
        # the server takes a signature once, and the same request twice in a millisecond would sign the same bytes
        with self._lock:
            self._last_ms = max(wall_ms + self.clock_offset_ms, self._last_ms + 1)
            return str(self._last_ms)

    def headers(self, wall_ms: int, method: str, path: str, query: str, body: str) -> dict[str, str]:
        stamp = self.now_ms(wall_ms)
        return {
            KEY_HEADER: self.settings.key,
            TIMESTAMP_HEADER: stamp,
            SIGNATURE_HEADER: sign(self.settings.secret, stamp, method, path, query, body),
        }


def encode_query(query: Mapping[str, Any] | None) -> str:
    if not query:
        return ""
    pairs = [
        (name, str(value).lower() if isinstance(value, bool) else str(value))
        for name, value in query.items()
        if value is not None
    ]
    return urlencode(pairs, quote_via=quote)


def build_request(
    client: httpx.Client | httpx.AsyncClient, signer: Signer, call: Call[Any], wall_ms: int
) -> httpx.Request:
    settings = signer.settings
    body = "" if call.body is None else json.dumps(call.body, separators=(",", ":"))
    query = encode_query(call.query)
    url = httpx.URL(settings.url + API_PREFIX + call.path + (f"?{query}" if query else ""))
    # what is signed is what httpx will put on the wire, after its own normalising
    raw_path, _, raw_query = url.raw_path.decode().partition("?")
    headers = {"Content-Type": "application/json"} if body else {}
    if call.signed:
        headers.update(signer.headers(wall_ms, call.method, raw_path, raw_query, body))
    if call.mutating:
        if call.idempotency_key is None:
            call.idempotency_key = new_idempotency_key()
        headers["Idempotency-Key"] = call.idempotency_key
    return client.build_request(call.method, url, content=body or None, headers=headers)


def parse_retry_after(response: httpx.Response) -> float | None:
    text = response.headers.get("Retry-After")
    if text is None:
        return None
    try:
        return max(float(text), 0.0)
    except ValueError:
        return None


def error_of(response: httpx.Response) -> APIError:
    try:
        problem = response.json()
        if not isinstance(problem, dict):
            problem = {}
    except ValueError:
        problem = {}
    problem.setdefault("request_id", response.headers.get("X-Request-Id"))
    return error_from_problem(response.status_code, problem, parse_retry_after(response))


def should_retry(call: Call[Any], error: APIError) -> bool:
    # another process on this key signed the same bytes in the same millisecond; a new timestamp is a new signature
    if error.code in ("idempotency-in-flight", "replayed"):
        return True
    # a refusal with one of these statuses means nothing happened, so even a mutation is safe to send again
    return error.status in RETRIABLE_STATUS


def backoff_s(attempt: int, retry_after: float | None) -> float:
    if retry_after is not None:
        return retry_after
    return float(min(0.1 * (2**attempt), 5.0) * (0.5 + random.random() / 2))


def report_request(settings: Settings, call: Call[Any], request: httpx.Request, attempt: int) -> None:
    info = RequestInfo(call.method, str(request.url), attempt, call.idempotency_key)
    logger.debug("%s %s attempt=%d", info.method, info.url, info.attempt)
    if settings.hooks.on_request is not None:
        settings.hooks.on_request(info)


def report_response(
    settings: Settings, call: Call[Any], response: httpx.Response, attempt: int, elapsed: float
) -> None:
    remaining = response.headers.get("RateLimit-Remaining")
    info = ResponseInfo(
        call.method,
        str(response.request.url),
        attempt,
        response.status_code,
        response.headers.get("X-Request-Id"),
        elapsed,
        response.headers.get("Idempotent-Replay") == "true",
        int(remaining) if remaining is not None and remaining.isdigit() else None,
    )
    logger.debug("%s %s -> %d in %.3fs", info.method, info.url, info.status, info.elapsed_s)
    if settings.hooks.on_response is not None:
        settings.hooks.on_response(info)


def is_skew_refusal(status: int, body: bytes | bytearray | None) -> bool:
    return status == 401 and body is not None and b"timestamp-skew" in body


def offset_from(served: Any, before_s: float, after_s: float) -> int:
    return int(served["server_time_ms"]) - int((before_s + after_s) * 500)


def stream_url(settings: Settings) -> str:
    base = settings.url
    if base.startswith("https://"):
        return "wss://" + base[len("https://") :] + API_PREFIX + "/stream"
    if base.startswith("http://"):
        return "ws://" + base[len("http://") :] + API_PREFIX + "/stream"
    return base + API_PREFIX + "/stream"
