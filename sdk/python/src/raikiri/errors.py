from __future__ import annotations

from typing import Any


class RaikiriError(Exception):
    """Base class of everything this package raises."""


class TransportError(RaikiriError):
    """The request never produced an answer from the server."""


class StreamClosed(RaikiriError):
    """The stream was closed by this client."""


class APIError(RaikiriError):
    """The server refused the call. ``code`` is stable; ``title`` and ``detail`` may change."""

    def __init__(
        self,
        status: int,
        code: str = "",
        title: str = "",
        detail: str | None = None,
        request_id: str | None = None,
        retry_after: float | None = None,
    ) -> None:
        super().__init__(f"{status} {code or title}" + (f": {detail}" if detail else ""))
        self.status = status
        self.code = code
        self.title = title
        self.detail = detail
        self.request_id = request_id
        self.retry_after = retry_after


class AuthenticationError(APIError):
    """The key, signature, timestamp or replay check failed."""


class TimestampSkew(AuthenticationError):
    """This machine's clock is outside the server's window. The client resyncs and retries once."""


class PermissionDenied(APIError):
    """The key lacks the read, trade or algo permission."""


class OutOfScope(PermissionDenied):
    """The key is not allowed that account, symbol or address."""


class NotFound(APIError):
    """No such record, or one that belongs to another client."""


class Conflict(APIError):
    """The record is not in a state for that."""


class OutcomeUnknown(APIError):
    """The call may have happened. Look at your orders before sending it again with a new key."""


class IdempotencyMismatch(APIError):
    """This idempotency key was used for a different request."""


class OrderRejected(APIError):
    """The desk refused the order before it was queued: off tick, off lot, unknown instrument."""


class AlgoLost(APIError):
    """The execution service restarted and forgot this algo; its children remain ordinary orders."""


class BadRequest(APIError):
    """The server could not read the request."""


class RateLimited(APIError):
    """Over this key's rate limit. ``retry_after`` says how long to wait, in seconds."""


class ServerError(APIError):
    """The server failed, or something behind it is away."""


_BY_CODE: dict[str, type[APIError]] = {
    "auth-failed": AuthenticationError,
    "auth-failures": RateLimited,
    "replayed": AuthenticationError,
    "timestamp-skew": TimestampSkew,
    "permission-denied": PermissionDenied,
    "out-of-scope": OutOfScope,
    "ip-not-allowed": OutOfScope,
    "not-found": NotFound,
    "outcome-unknown": OutcomeUnknown,
    "backend-timeout": OutcomeUnknown,
    "idempotency-mismatch": IdempotencyMismatch,
    "order-refused": OrderRejected,
    "unknown-instrument": OrderRejected,
    "account-mismatch": OrderRejected,
    "bad-qty": OrderRejected,
    "algo-lost": AlgoLost,
    "external-ref-too-long": OrderRejected,
    "algo-refused": OrderRejected,
    "rate-limited": RateLimited,
    "too-many-streams": RateLimited,
}


def error_from_problem(status: int, problem: dict[str, Any], retry_after: float | None) -> APIError:
    code = str(problem.get("code", ""))
    kind = _BY_CODE.get(code)
    if kind is None:
        if status == 401:
            kind = AuthenticationError
        elif status == 403:
            kind = PermissionDenied
        elif status == 404:
            kind = NotFound
        elif status == 409:
            kind = Conflict
        elif status == 429:
            kind = RateLimited
        elif status >= 500:
            kind = ServerError
        else:
            kind = BadRequest
    return kind(
        status,
        code,
        str(problem.get("title", "")),
        problem.get("detail"),
        problem.get("request_id"),
        retry_after,
    )
