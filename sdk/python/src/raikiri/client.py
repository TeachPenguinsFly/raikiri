from __future__ import annotations

import time
from collections.abc import Iterator, Sequence
from decimal import Decimal
from types import TracebackType
from typing import Any, TypeVar

import httpx

from . import _calls as api
from . import models as m
from ._core import (
    Call,
    Hooks,
    Settings,
    Signer,
    backoff_s,
    build_request,
    error_of,
    report_request,
    report_response,
    settings_from_env,
    should_retry,
)
from ._stream_core import Subscriptions
from .errors import APIError, RaikiriError, TimestampSkew, TransportError
from .models import DecimalLike, InstrumentRef, Side
from .stream import Stream

T = TypeVar("T")


class _Runner:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.signer = Signer(settings)
        self.http = httpx.Client(timeout=settings.timeout)

    def run(self, call: Call[T]) -> T:
        resynced = False
        attempt = 0
        while True:
            request = build_request(self.http, self.signer, call, int(time.time() * 1000))
            report_request(self.settings, call, request, attempt)
            started = time.monotonic()
            try:
                response = self.http.send(request)
            except httpx.TransportError as failure:
                if attempt >= self.settings.retries:
                    raise TransportError(str(failure)) from failure
                time.sleep(backoff_s(attempt, None))
                attempt += 1
                continue
            report_response(self.settings, call, response, attempt, time.monotonic() - started)
            if response.status_code < 300:
                return call.parse(response.json())
            error = error_of(response)
            if isinstance(error, TimestampSkew) and not resynced:
                resynced = True
                self.sync_clock()
                continue
            if should_retry(call, error) and attempt < self.settings.retries:
                time.sleep(backoff_s(attempt, error.retry_after))
                attempt += 1
                continue
            raise error

    def sync_clock(self) -> None:
        before = time.time()
        served = self.run(api.server_time())
        after = time.time()
        self.signer.clock_offset_ms = served - int((before + after) * 500)


class Orders:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    def place(
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
        """Queue an order. The ack means the desk took it; follow the order or the stream for the venue's answer.

        With no ``idempotency_key`` one is made per call and kept across retries of that call. ``external_ref``, up
        to 40 characters, is your own reference and comes back on the order. If the server's checks refuse the order
        after this ack it goes to ``denied``, and ``Order.refusal`` says why.
        """
        return self._run(
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

    def limit(
        self, instrument: InstrumentRef, side: Side | str, qty: DecimalLike, price: DecimalLike, **options: Any
    ) -> m.OrderAck:
        """A limit order. Takes the same options as ``place``."""
        return self.place(instrument, side, qty, price=price, order_type="limit", **options)

    def market(self, instrument: InstrumentRef, side: Side | str, qty: DecimalLike, **options: Any) -> m.OrderAck:
        """A market order. Takes the same options as ``place``."""
        return self.place(instrument, side, qty, order_type="market", **options)

    def cancel(self, order_id: str, *, idempotency_key: str | None = None) -> m.CancelAck:
        """Queue a cancel. It is not confirmed until the order says so; it can still fill meanwhile."""
        return self._run(api.cancel(order_id, idempotency_key))

    def amend(
        self,
        order_id: str,
        *,
        price: DecimalLike | None = None,
        qty: DecimalLike | None = None,
        idempotency_key: str | None = None,
    ) -> m.OrderAck:
        """Cancel, wait for the cancel to be confirmed, then place what remains.

        The new order has a new id, and ``qty`` counts what already filled. Between the two steps nothing rests,
        so it gives up queue position; a venue-side modify is not offered.
        """
        return self._run(api.amend(order_id, price=price, qty=qty, idempotency_key=idempotency_key))

    def cancel_all(
        self,
        *,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        idempotency_key: str | None = None,
    ) -> m.CancelAll:
        """Cancel this client's open orders, leaving algo children to their algos."""
        return self._run(api.cancel_all(account=account, instrument=instrument, idempotency_key=idempotency_key))

    def get(self, order_id: str) -> m.Order:
        return self._run(api.order(order_id))

    def events(self, order_id: str) -> list[m.OrderEvent]:
        """Every lifecycle event of this order, oldest first."""
        return self._run(api.order_events(order_id))

    def list(
        self,
        *,
        open: bool | None = None,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        since: int | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Order]:
        """One page, newest first. A page can be short and still have a ``next_cursor``."""
        return self._run(
            api.orders(open=open, account=account, instrument=instrument, since=since, cursor=cursor, limit=limit)
        )

    def iter(self, **filters: Any) -> Iterator[m.Order]:
        """Every order that matches, across pages."""
        cursor = None
        while True:
            page = self.list(cursor=cursor, **filters)
            yield from page.items
            cursor = page.next_cursor
            if cursor is None:
                return

    def wait(self, order_id: str, *, timeout: float = 10.0, poll: float = 0.1) -> m.Order:
        """Poll until the order is closed; ``TimeoutError`` if it is still open after ``timeout`` seconds."""
        deadline = time.monotonic() + timeout
        while True:
            order = self.get(order_id)
            if not order.is_open:
                return order
            if time.monotonic() >= deadline:
                raise TimeoutError(f"order {order_id} is still {order.status}")
            time.sleep(poll)


class AccountApi:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    def accounts(self) -> list[m.Account]:
        return self._run(api.accounts())

    def balances(self, *, account: str | None = None) -> list[m.AccountBalances]:
        """As each venue last reported them. ``stale`` is set when that is too long ago to trust."""
        return self._run(api.balances(account=account))

    def positions(self, *, account: str | None = None, instrument: InstrumentRef | None = None) -> list[m.Position]:
        """This client's non-flat positions, folded from its own fills."""
        return self._run(api.positions(account=account, instrument=instrument))

    def fills(
        self,
        *,
        account: str | None = None,
        instrument: InstrumentRef | None = None,
        since: int | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Fill]:
        return self._run(api.fills(account=account, instrument=instrument, since=since, cursor=cursor, limit=limit))

    def iter_fills(self, **filters: Any) -> Iterator[m.Fill]:
        cursor = None
        while True:
            page = self.fills(cursor=cursor, **filters)
            yield from page.items
            cursor = page.next_cursor
            if cursor is None:
                return


class MarketApi:
    def __init__(self, runner: _Runner, open_stream: Any) -> None:
        self._run = runner.run
        self._open_stream = open_stream

    def instrument(self, instrument_id: str) -> m.Instrument:
        return self._run(api.instrument(instrument_id))

    def instruments(
        self,
        *,
        account: str | None = None,
        kind: str | None = None,
        cursor: str | None = None,
        limit: int | None = None,
    ) -> m.Page[m.Instrument]:
        return self._run(api.instruments(account=account, kind=kind, cursor=cursor, limit=limit))

    def markets(self, query: str | None = None, *, limit: int | None = None) -> list[m.Market]:
        """Each asset with every listing of it, across venues."""
        return self._run(api.markets(query, limit=limit))

    def find(self, symbol: str, *, account: str | None = None, market: str | None = None) -> m.Instrument:
        """The one instrument with this local symbol, such as ``BTC/USDT``. Name an account when several list it."""
        cursor = None
        matches = []
        while True:
            page = self.instruments(account=account, kind=market, cursor=cursor, limit=500)
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

    def quote(self, instrument: InstrumentRef, *, timeout: float = 5.0) -> m.Quote:
        """The current quote, taken from the stream. For more than one, keep a stream open."""
        with self._open_stream(quotes=instrument) as stream:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                event = stream.next(timeout=max(deadline - time.monotonic(), 0.0))
                if isinstance(event, m.Quote):
                    return event
        raise TimeoutError(f"no quote for {m.instrument_id(instrument)} in {timeout}s")


class OptionsApi:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    def underlyings(self) -> list[m.OptionUnderlying]:
        return self._run(api.option_underlyings())

    def chain(self, underlying: str, expiry: str, *, venue: str | None = None) -> m.OptionChain:
        """One expiry across venues, with each listing's greeks. ``expiry`` is ``YYYYMMDD``."""
        return self._run(api.option_chain(underlying, expiry, venue=venue))

    def surface(self, underlying: str, *, venue: str | None = None) -> dict[str, Any]:
        return self._run(api.option_surface(underlying, venue=venue))


class Algo:
    """A running parent order. ``state`` is as of the last ``refresh``, ``pause``, ``resume`` or ``cancel``."""

    def __init__(self, api_: Algos, state: m.AlgoOrder) -> None:
        self._api = api_
        self.state = state

    @property
    def id(self) -> str:
        return self.state.id

    def refresh(self) -> m.AlgoOrder:
        self.state = self._api.get(self.id)
        return self.state

    def pause(self) -> m.AlgoOrder:
        self.state = self._api.pause(self.id)
        return self.state

    def resume(self) -> m.AlgoOrder:
        self.state = self._api.resume(self.id)
        return self.state

    def cancel(self) -> m.AlgoOrder:
        self.state = self._api.cancel(self.id)
        return self.state

    def watch(self, *, interval: float = 1.0) -> Iterator[m.AlgoOrder]:
        """Yield the parent each time it changes, and stop once it is done."""
        last: tuple[Any, ...] | None = None
        while True:
            state = self.refresh()
            now = (state.status, state.filled, state.working, state.slices_fired)
            if now != last:
                last = now
                yield state
            if state.done:
                return
            time.sleep(interval)

    def wait(self, *, interval: float = 1.0) -> m.AlgoOrder:
        for state in self.watch(interval=interval):
            if state.done:
                return state
        return self.state


class Algos:
    def __init__(self, runner: _Runner) -> None:
        self._run = runner.run

    def twap(self, symbol: str, side: Side | str, *, horizon_s: int, slices: int, **options: Any) -> Algo:
        """Slice evenly over ``horizon_s``. Give ``qty`` or ``notional``; ``limit`` caps every child."""
        return self._start("twap", symbol, side, horizon_s=horizon_s, slices=slices, **options)

    def vwap(
        self, symbol: str, side: Side | str, *, horizon_s: int, slices: int, profile: str = "flat", **options: Any
    ) -> Algo:
        """Slice along a volume curve: ``flat``, ``u`` or ``recorded``."""
        return self._start("vwap", symbol, side, horizon_s=horizon_s, slices=slices, profile=profile, **options)

    def sor(self, symbol: str, side: Side | str, **options: Any) -> Algo:
        """Send it now across the venues that list the symbol, best price after fees first."""
        return self._start("sor", symbol, side, **options)

    def pov(self, symbol: str, side: Side | str, *, pov_bp: int, horizon_s: int = 0, **options: Any) -> Algo:
        """Trade ``pov_bp`` of market volume; 1000 is 10 percent."""
        return self._start("pov", symbol, side, pov_bp=pov_bp, horizon_s=horizon_s, **options)

    def _start(self, kind: str, symbol: str, side: Side | str, **options: Any) -> Algo:
        return Algo(self, self._run(api.start_algo(kind, symbol, side, **options)))

    def get(self, algo_id: str) -> m.AlgoOrder:
        return self._run(api.algo(algo_id))

    def attach(self, algo_id: str) -> Algo:
        """A handle on an algo started earlier."""
        return Algo(self, self.get(algo_id))

    def list(self) -> list[m.AlgoOrder]:
        return self._run(api.algos())

    def pause(self, algo_id: str) -> m.AlgoOrder:
        return self._run(api.algo_action(algo_id, "pause"))

    def resume(self, algo_id: str) -> m.AlgoOrder:
        return self._run(api.algo_action(algo_id, "resume"))

    def cancel(self, algo_id: str) -> m.AlgoOrder:
        return self._run(api.algo_action(algo_id, "cancel"))


class Client:
    """The synchronous client.

    Reads ``RAIKIRI_API_URL``, ``RAIKIRI_API_KEY`` and ``RAIKIRI_API_SECRET`` for what is not passed. With no URL it
    talks to the paper endpoint on loopback.

    Reads, and mutations with their idempotency key, are retried on network errors, 429 and 503. A call the server
    may have carried out without answering raises ``OutcomeUnknown`` instead of being sent twice.
    """

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
        self.orders = Orders(self._runner)
        self.account = AccountApi(self._runner)
        self.market = MarketApi(self._runner, self.stream)
        self.options = OptionsApi(self._runner)
        self.algos = Algos(self._runner)

    def stream(
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
        reconnect: bool = True,
    ) -> Stream:
        """Open a stream: ``for event in client.stream(quotes=btc, fills=True): ...``"""
        stream = Stream(self._runner.signer, Subscriptions(), reconnect=reconnect)
        return stream.subscribe(
            quotes=quotes,
            book=book,
            levels=levels,
            greeks=greeks,
            funding=funding,
            ticker=ticker,
            hz=hz,
            orders=orders,
            fills=fills,
            positions=positions,
            balances=balances,
            algos=algos,
        )

    def call(
        self,
        method: str,
        path: str,
        *,
        query: dict[str, Any] | None = None,
        body: Any = None,
        idempotency_key: str | None = None,
    ) -> Any:
        """A call with no method here, such as an extension under ``/x/``. Signed and retried like the rest."""
        return self._runner.run(Call(method, path, lambda data: data, query, body, idempotency_key))

    def whoami(self) -> m.Whoami:
        """What this key is allowed."""
        return self._runner.run(api.whoami())

    def server_time_ms(self) -> int:
        return self._runner.run(api.server_time())

    def sync_clock(self) -> None:
        """Measure this machine's offset from the server. It also happens by itself on a skew refusal."""
        self._runner.sync_clock()

    def close(self) -> None:
        self._runner.http.close()

    def __enter__(self) -> Client:
        return self

    def __exit__(
        self, kind: type[BaseException] | None, value: BaseException | None, trace: TracebackType | None
    ) -> None:
        self.close()


__all__ = ["APIError", "Algo", "Algos", "Client"]
