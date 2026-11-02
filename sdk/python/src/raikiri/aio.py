from __future__ import annotations

import asyncio
import json
import logging
import random
import time
from collections.abc import AsyncIterator, Sequence
from decimal import Decimal
from types import TracebackType
from typing import Any, TypeVar

import httpx
from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed, InvalidHandshake, InvalidStatus

from . import _calls as api
from . import models as m
from ._core import (
    API_PREFIX,
    Call,
    Hooks,
    Settings,
    Signer,
    backoff_s,
    build_request,
    error_of,
    is_skew_refusal,
    offset_from,
    report_request,
    report_response,
    settings_from_env,
    should_retry,
    stream_url,
)
from ._stream_core import StreamState, Subscriptions, channels_of
from .errors import RaikiriError, TimestampSkew, TransportError, error_from_problem
from .models import DecimalLike, InstrumentRef, Side, StreamEvent
from .stream import MAX_BACKOFF_S, PATH, refused_for_good

logger = logging.getLogger("raikiri.stream")

T = TypeVar("T")


class _Runner:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.signer = Signer(settings)
        self.http = httpx.AsyncClient(timeout=settings.timeout)

    async def run(self, call: Call[T]) -> T:
        resynced = False
        attempt = 0
        while True:
            request = build_request(self.http, self.signer, call, int(time.time() * 1000))
            report_request(self.settings, call, request, attempt)
            started = time.monotonic()
            try:
                response = await self.http.send(request)
            except httpx.TransportError as failure:
                if attempt >= self.settings.retries:
                    raise TransportError(str(failure)) from failure
                await asyncio.sleep(backoff_s(attempt, None))
                attempt += 1
                continue
            report_response(self.settings, call, response, attempt, time.monotonic() - started)
            if response.status_code < 300:
                return call.parse(response.json())
            error = error_of(response)
            if isinstance(error, TimestampSkew) and not resynced:
                resynced = True
                await self.sync_clock()
                continue
            if should_retry(call, error) and attempt < self.settings.retries:
                await asyncio.sleep(backoff_s(attempt, error.retry_after))
                attempt += 1
                continue
            raise error

    async def sync_clock(self) -> None:
        before = time.time()
        served = await self.run(api.server_time())
        after = time.time()
        self.signer.clock_offset_ms = served - int((before + after) * 500)


class AsyncStream:
    """The async twin of ``Stream``: ``async for event in client.stream(fills=True): ...``"""

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

    async def subscribe(
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
    ) -> AsyncStream:
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
                    await self._send(private)
            if message is not None:
                await self._send(message)
        return self

    def __aiter__(self) -> AsyncIterator[StreamEvent]:
        return self

    async def __anext__(self) -> StreamEvent:
        event = await self.next()
        if event is None:
            raise StopAsyncIteration
        return event

    async def next(self, timeout: float | None = None) -> StreamEvent | None:
        """The next event, or None when ``timeout`` seconds pass without one or the stream ended."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            if self._pending:
                return self._pending.pop(0)
            if self._closed:
                return None
            if self._socket is None:
                if not await self._connect(deadline):
                    return None
                continue
            silence = self._state.heartbeat_s * 3
            wait = silence if deadline is None else min(silence, max(deadline - time.monotonic(), 0.0))
            try:
                text = await asyncio.wait_for(self._socket.recv(), wait)
            except (asyncio.TimeoutError, TimeoutError):
                if deadline is not None and time.monotonic() >= deadline:
                    return None
                logger.warning("no heartbeat for %.0fs; reconnecting", silence)
                await self._lost()
                continue
            except ConnectionClosed:
                await self._lost()
                continue
            events, sends, problem = self._state.on_frame(text if isinstance(text, str) else text.decode())
            for message in sends:
                await self._send(message)
            if problem is not None:
                raise error_from_problem(int(problem.get("status", 400)), problem, None)
            self._pending.extend(events)

    async def _send(self, message: dict[str, Any]) -> None:
        if self._socket is None:
            return
        try:
            await self._socket.send(json.dumps(message, separators=(",", ":")))
        except ConnectionClosed:
            await self._lost()

    async def _lost(self) -> None:
        socket, self._socket = self._socket, None
        if socket is not None:
            try:
                await socket.close()
            except Exception:  # noqa: BLE001 - it is gone either way
                pass
        if not self._reconnect:
            self._closed = True

    async def _connect(self, deadline: float | None) -> bool:
        while not self._closed:
            try:
                headers = self._signer.headers(int(time.time() * 1000), "GET", PATH, "", "")
                self._socket = await connect(
                    stream_url(self._signer.settings),
                    additional_headers=headers,
                    open_timeout=10,
                    close_timeout=2,
                    max_size=None,
                )
                if self._ever_open:
                    self.reconnects += 1
                self._ever_open = True
                self._attempt = 0
                return True
            except (OSError, InvalidHandshake, TimeoutError, asyncio.TimeoutError) as error:
                if isinstance(error, InvalidStatus) and not self._resynced and await self._resync(error):
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
                if deadline is not None and time.monotonic() + pause >= deadline:
                    await asyncio.sleep(max(deadline - time.monotonic(), 0.0))
                    return False
                await asyncio.sleep(pause)
        return False

    async def _resync(self, refusal: InvalidStatus) -> bool:
        if not is_skew_refusal(refusal.response.status_code, refusal.response.body):
            return False
        self._resynced = True
        settings = self._signer.settings
        before = time.time()
        try:
            async with httpx.AsyncClient(timeout=settings.timeout) as http:
                served = (await http.get(settings.url + API_PREFIX + "/time")).json()
            self._signer.clock_offset_ms = offset_from(served, before, time.time())
        except (httpx.HTTPError, ValueError, KeyError, TypeError):
            return False
        return True

    async def close(self) -> None:
        self._closed = True
        socket, self._socket = self._socket, None
        if socket is not None:
            await socket.close()

    async def __aenter__(self) -> AsyncStream:
        return self

    async def __aexit__(
        self, kind: type[BaseException] | None, value: BaseException | None, trace: TracebackType | None
    ) -> None:
        await self.close()


class AsyncOrders:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    async def place(
        self,
        instrument: InstrumentRef,
        side: Side | str,
        qty: DecimalLike,
        *,
        price: DecimalLike | None = None,
        order_type: str | None = None,
        tif: str = "gtc",
        post_only: bool = False,
        reduce_only: bool = False,
        account: str | None = None,
        external_ref: str | None = None,
        idempotency_key: str | None = None,
    ) -> m.OrderAck:
        """See ``Orders.place``."""
        return await self._run(
            api.place(
                instrument,
                side,
                qty,
                price=price,
                order_type=order_type,
                tif=tif,
                post_only=post_only,
                reduce_only=reduce_only,
                account=account,
                external_ref=external_ref,
                idempotency_key=idempotency_key,
            )
        )

    async def limit(
        self, instrument: InstrumentRef, side: Side | str, qty: DecimalLike, price: DecimalLike, **options: Any
    ) -> m.OrderAck:
        return await self.place(instrument, side, qty, price=price, order_type="limit", **options)

    async def market(self, instrument: InstrumentRef, side: Side | str, qty: DecimalLike, **options: Any) -> m.OrderAck:
        return await self.place(instrument, side, qty, order_type="market", **options)

    async def cancel(self, order_id: str, *, idempotency_key: str | None = None) -> m.CancelAck:
        return await self._run(api.cancel(order_id, idempotency_key))

    async def amend(
        self,
        order_id: str,
        *,
        price: DecimalLike | None = None,
        qty: DecimalLike | None = None,
        idempotency_key: str | None = None,
    ) -> m.OrderAck:
        """See ``Orders.amend``."""
        return await self._run(api.amend(order_id, price=price, qty=qty, idempotency_key=idempotency_key))

    async def cancel_all(
        self,
        *,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        idempotency_key: str | None = None,
    ) -> m.CancelAll:
        return await self._run(api.cancel_all(account=account, instrument=instrument, idempotency_key=idempotency_key))

    async def get(self, order_id: str) -> m.Order:
        return await self._run(api.order(order_id))

    async def events(self, order_id: str) -> list[m.OrderEvent]:
        return await self._run(api.order_events(order_id))

    async def list(
        self,
        *,
        open: bool | None = None,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        since: int | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Order]:
        return await self._run(
            api.orders(open=open, account=account, instrument=instrument, since=since, cursor=cursor, limit=limit)
        )

    async def iter(self, **filters: Any) -> AsyncIterator[m.Order]:
        cursor = None
        while True:
            page = await self.list(cursor=cursor, **filters)
            for order in page.items:
                yield order
            cursor = page.next_cursor
            if cursor is None:
                return


class AsyncAccountApi:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    async def accounts(self) -> list[m.Account]:
        return await self._run(api.accounts())

    async def balances(self, *, account: str | None = None) -> list[m.AccountBalances]:
        return await self._run(api.balances(account=account))

    async def positions(
        self, *, account: str | None = None, instrument: InstrumentRef | None = None
    ) -> list[m.Position]:
        return await self._run(api.positions(account=account, instrument=instrument))

    async def fills(
        self,
        *,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        since: int | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Fill]:
        return await self._run(
            api.fills(account=account, instrument=instrument, since=since, cursor=cursor, limit=limit)
        )


class AsyncMarketApi:
    def __init__(self, runner: _Runner, open_stream: Any) -> None:
        self._run = runner.run
        self._open_stream = open_stream

    async def instrument(self, instrument_id: str) -> m.Instrument:
        return await self._run(api.instrument(instrument_id))

    async def instruments(
        self,
        *,
        account: str | None = None,
        kind: str | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Instrument]:
        return await self._run(api.instruments(account=account, kind=kind, cursor=cursor, limit=limit))

    async def markets(self, query: str | None = None, *, limit: int | None = None) -> list[m.Market]:
        return await self._run(api.markets(query, limit=limit))

    async def find(self, symbol: str, *, account: str | None = None, market: str | None = None) -> m.Instrument:
        """See ``MarketApi.find``."""
        cursor = None
        matches = []
        while True:
            page = await self.instruments(account=account, kind=market, cursor=cursor, limit=500)
            matches += [row for row in page.items if symbol in (row.local_symbol, row.exchange_symbol)]
            cursor = page.next_cursor
            if cursor is None:
                break
        if not matches:
            raise RaikiriError(f"no instrument {symbol} on {account or 'any account'}")
        if len(matches) > 1:
            names = ", ".join(row.account for row in matches)
            raise RaikiriError(f"{symbol} is listed on {names}; name an account")
        return matches[0]

    async def quote(self, instrument: InstrumentRef, *, timeout: float = 5.0) -> m.Quote:
        async with await self._open_stream(quotes=instrument) as stream:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                event = await stream.next(timeout=max(deadline - time.monotonic(), 0.0))
                if isinstance(event, m.Quote):
                    return event
        raise TimeoutError(f"no quote for {m.instrument_id(instrument)} in {timeout}s")


class AsyncOptionsApi:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    async def underlyings(self) -> list[m.OptionUnderlying]:
        return await self._run(api.option_underlyings())

    async def chain(self, underlying: str, expiry: str, *, venue: str | None = None) -> m.OptionChain:
        return await self._run(api.option_chain(underlying, expiry, venue=venue))

    async def surface(self, underlying: str, *, venue: str | None = None) -> dict[str, Any]:
        return await self._run(api.option_surface(underlying, venue=venue))


class AsyncAlgo:
    def __init__(self, api_: AsyncAlgos, state: m.AlgoOrder) -> None:
        self._api = api_
        self.state = state

    @property
    def id(self) -> str:
        return self.state.id

    async def refresh(self) -> m.AlgoOrder:
        self.state = await self._api.get(self.id)
        return self.state

    async def pause(self) -> m.AlgoOrder:
        self.state = await self._api.pause(self.id)
        return self.state

    async def resume(self) -> m.AlgoOrder:
        self.state = await self._api.resume(self.id)
        return self.state

    async def cancel(self) -> m.AlgoOrder:
        self.state = await self._api.cancel(self.id)
        return self.state

    async def watch(self, *, interval: float = 1.0) -> AsyncIterator[m.AlgoOrder]:
        last: tuple[Any, ...] | None = None
        while True:
            state = await self.refresh()
            now = (state.status, state.filled, state.working, state.slices_fired)
            if now != last:
                last = now
                yield state
            if state.done:
                return
            await asyncio.sleep(interval)


class AsyncAlgos:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    async def twap(self, symbol: str, side: Side | str, *, horizon_s: int, slices: int, **options: Any) -> AsyncAlgo:
        return await self._start("twap", symbol, side, horizon_s=horizon_s, slices=slices, **options)

    async def vwap(
        self, symbol: str, side: Side | str, *, horizon_s: int, slices: int, profile: str = "flat", **options: Any
    ) -> AsyncAlgo:
        return await self._start("vwap", symbol, side, horizon_s=horizon_s, slices=slices, profile=profile, **options)

    async def sor(self, symbol: str, side: Side | str, **options: Any) -> AsyncAlgo:
        return await self._start("sor", symbol, side, **options)

    async def pov(self, symbol: str, side: Side | str, *, pov_bp: int, horizon_s: int = 0, **options: Any) -> AsyncAlgo:
        return await self._start("pov", symbol, side, pov_bp=pov_bp, horizon_s=horizon_s, **options)

    async def _start(self, kind: str, symbol: str, side: Side | str, **options: Any) -> AsyncAlgo:
        return AsyncAlgo(self, await self._run(api.start_algo(kind, symbol, side, **options)))

    async def get(self, algo_id: str) -> m.AlgoOrder:
        return await self._run(api.algo(algo_id))

    async def list(self) -> list[m.AlgoOrder]:
        return await self._run(api.algos())

    async def pause(self, algo_id: str) -> m.AlgoOrder:
        return await self._run(api.algo_action(algo_id, "pause"))

    async def resume(self, algo_id: str) -> m.AlgoOrder:
        return await self._run(api.algo_action(algo_id, "resume"))

    async def cancel(self, algo_id: str) -> m.AlgoOrder:
        return await self._run(api.algo_action(algo_id, "cancel"))


class AsyncClient:
    """The asyncio client, with the same surface and the same retry rules as ``Client``."""

    def __init__(
        self,
        url: str | None = None,
        *,
        key: str | None = None,
        secret: str | None = None,
        timeout: float = 10.0,
        retries: int = 3,
        hooks: Hooks | None = None,
    ) -> None:
        settings = settings_from_env(url, key, secret, timeout, retries, hooks)
        self._runner = _Runner(settings)
        self.orders = AsyncOrders(self._runner)
        self.account = AsyncAccountApi(self._runner)
        self.market = AsyncMarketApi(self._runner, self.stream)
        self.options = AsyncOptionsApi(self._runner)
        self.algos = AsyncAlgos(self._runner)

    async def stream(self, *, reconnect: bool = True, **subscriptions: Any) -> AsyncStream:
        """Open a stream: ``async for event in await client.stream(quotes=btc): ...``"""
        stream = AsyncStream(self._runner.signer, Subscriptions(), reconnect=reconnect)
        return await stream.subscribe(**subscriptions)

    async def call(
        self,
        method: str,
        path: str,
        *,
        query: dict[str, Any] | None = None,
        body: Any = None,
        idempotency_key: str | None = None,
    ) -> Any:
        """See ``Client.call``."""
        return await self._runner.run(Call(method, path, lambda data: data, query, body, idempotency_key))

    async def whoami(self) -> m.Whoami:
        return await self._runner.run(api.whoami())

    async def server_time_ms(self) -> int:
        return await self._runner.run(api.server_time())

    async def sync_clock(self) -> None:
        await self._runner.sync_clock()

    async def close(self) -> None:
        await self._runner.http.aclose()

    async def __aenter__(self) -> AsyncClient:
        return self

    async def __aexit__(
        self, kind: type[BaseException] | None, value: BaseException | None, trace: TracebackType | None
    ) -> None:
        await self.close()
