from __future__ import annotations

import json
import logging
import random
import time
from collections.abc import Iterator, Sequence
from decimal import Decimal
from types import TracebackType
from typing import Any

import httpx
from websockets.exceptions import ConnectionClosed, InvalidHandshake, InvalidStatus
from websockets.sync.client import ClientConnection, connect

from ._core import API_PREFIX, Signer, is_skew_refusal, offset_from, stream_url
from ._stream_core import StreamState, Subscriptions, channels_of
from .errors import APIError, RaikiriError, StreamClosed, error_from_problem
from .models import InstrumentRef, StreamEvent

logger = logging.getLogger("raikiri.stream")

MAX_BACKOFF_S = 30.0
PATH = API_PREFIX + "/stream"


def refused_for_good(error: Exception) -> APIError | None:
    """A refusal that reconnecting cannot cure comes back as an exception; anything else is retried."""
    if not isinstance(error, InvalidStatus):
        return None
    response = error.response
    status = response.status_code
    if status in (401, 403):
        try:
            problem = json.loads(bytes(response.body or b""))
        except ValueError:
            problem = {}
        return error_from_problem(status, problem, None)
    return None


class Stream:
    """A websocket that reconnects, resubscribes and resumes from its last sequence on its own.

    Iterate it for typed events. Quotes, books, greeks, funding and tickers are the current value and may skip;
    orders, fills, positions, balances and algos are sequenced, and a ``Gap`` says when some could not be resent.
    """

    def __init__(self, signer: Signer, subscriptions: Subscriptions, *, reconnect: bool = True) -> None:
        self._signer = signer
        self._state = StreamState(subscriptions)
        self._reconnect = reconnect
        self._socket: ClientConnection | None = None
        self._closed = False
        self._pending: list[StreamEvent] = []
        self._attempt = 0
        self._ever_open = False
        self._resynced = False
        self.reconnects = 0

    @property
    def last_seq(self) -> int | None:
        return self._state.last_seq

    def subscribe(
        self,
        *,
        quotes: Sequence[InstrumentRef] | InstrumentRef | None = None,
        book: InstrumentRef | None = None,
        levels: int | None = None,
        greeks: Sequence[InstrumentRef] | InstrumentRef | None = None,
        funding: Sequence[InstrumentRef] | InstrumentRef | None = None,
        ticker: Sequence[InstrumentRef] | InstrumentRef | None = None,
        hz: int | Decimal | None = None,
        orders: bool = False,
        fills: bool = False,
        positions: bool = False,
        balances: bool = False,
        algos: bool = False,
    ) -> Stream:
        """Add subscriptions. They are sent now if connected, and again after every reconnect."""
        subs = self._state.subscriptions
        added = channels_of(orders, fills, positions, balances, algos) - subs.channels
        subs.channels |= added
        message = subs.market_message(
            quotes=quotes, book=book, levels=levels, greeks=greeks, funding=funding, ticker=ticker, hz=hz
        )
        if self._socket is not None:
            if added:
                private = self._state.private_message()
                if private is not None:
                    self._send(private)
            if message is not None:
                self._send(message)
        return self

    def __iter__(self) -> Iterator[StreamEvent]:
        return self

    def __next__(self) -> StreamEvent:
        event = self.next(timeout=None)
        if event is None:
            raise StopIteration
        return event

    def next(self, timeout: float | None = None) -> StreamEvent | None:
        """The next event, or None when ``timeout`` seconds pass without one or the stream ended."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self._pending:
                return self._pending.pop(0)
            if self._closed:
                return None
            if self._socket is None:
                if not self._connect(deadline):
                    return None
                continue
            assert self._socket is not None
            silence = self._state.heartbeat_s * 3
            wait = silence if deadline is None else min(silence, max(deadline - time.monotonic(), 0.0))
            try:
                text = self._socket.recv(timeout=wait)
            except TimeoutError:
                if deadline is not None and time.monotonic() >= deadline:
                    return None
                logger.warning("no heartbeat for %.0fs; reconnecting", silence)
                self._lost()
                continue
            except ConnectionClosed:
                self._lost()
                continue
            self._on_text(text if isinstance(text, str) else text.decode())

    def _on_text(self, text: str) -> None:
        events, sends, problem = self._state.on_frame(text)
        for message in sends:
            self._send(message)
        if problem is not None:
            raise error_from_problem(int(problem.get("status", 400)), problem, None)
        self._pending.extend(events)

    def _send(self, message: dict[str, Any]) -> None:
        assert self._socket is not None
        try:
            self._socket.send(json.dumps(message, separators=(",", ":")))
        except ConnectionClosed:
            self._lost()

    def _lost(self) -> None:
        socket, self._socket = self._socket, None
        if socket is not None:
            try:
                socket.close()
            except Exception:  # noqa: BLE001 - it is gone either way
                pass
        if not self._reconnect:
            self._closed = True

    def _connect(self, deadline: float | None) -> bool:
        while not self._closed:
            try:
                headers = self._signer.headers(int(time.time() * 1000), "GET", PATH, "", "")
                # entered by hand: this connection outlives any one block, and close() ends it
                self._socket = connect(
                    stream_url(self._signer.settings),
                    additional_headers=headers,
                    open_timeout=10,
                    close_timeout=2,
                    max_size=None,
                ).__enter__()
                if self._ever_open:
                    self.reconnects += 1
                self._ever_open = True
                self._attempt = 0
                return True
            except (OSError, InvalidHandshake, TimeoutError) as error:
                if isinstance(error, InvalidStatus) and not self._resynced and self._resync(error):
                    continue
                final = refused_for_good(error)
                if final is not None:
                    self._closed = True
                    raise final from error
                if not self._reconnect:
                    self._closed = True
                    raise RaikiriError(f"cannot open the stream: {error}") from error
                pause = min(0.2 * (2**self._attempt), MAX_BACKOFF_S) * (0.5 + random.random() / 2)
                self._attempt += 1
                logger.info("stream connect failed (%s); retrying in %.1fs", error, pause)
                if deadline is not None and time.monotonic() + pause >= deadline:
                    time.sleep(max(deadline - time.monotonic(), 0.0))
                    return False
                time.sleep(pause)
        return False

    def _resync(self, refusal: InvalidStatus) -> bool:
        if not is_skew_refusal(refusal.response.status_code, refusal.response.body):
            return False
        self._resynced = True
        settings = self._signer.settings
        before = time.time()
        try:
            served = httpx.get(settings.url + API_PREFIX + "/time", timeout=settings.timeout).json()
            self._signer.clock_offset_ms = offset_from(served, before, time.time())
        except (httpx.HTTPError, ValueError, KeyError, TypeError):
            return False
        return True

    def close(self) -> None:
        self._closed = True
        socket, self._socket = self._socket, None
        if socket is not None:
            socket.close()

    def __enter__(self) -> Stream:
        return self

    def __exit__(
        self, kind: type[BaseException] | None, value: BaseException | None, trace: TracebackType | None
    ) -> None:
        self.close()


__all__ = ["Stream", "StreamClosed"]
