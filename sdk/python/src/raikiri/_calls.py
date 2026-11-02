from __future__ import annotations

from collections.abc import Callable
from typing import Any, TypeVar

from . import models as m
from ._core import Call
from .models import DecimalLike, InstrumentRef, Side, build, decimal_text, instrument_id

T = TypeVar("T")


def _one(kind: type[T]) -> Callable[[Any], T]:
    return lambda data: build(kind, data)


def _items(kind: type[T]) -> Callable[[Any], list[T]]:
    return lambda data: [build(kind, row) for row in data.get("items", [])]


def _page(kind: type[T]) -> Callable[[Any], m.Page[T]]:
    return lambda data: m.Page([build(kind, row) for row in data.get("items", [])], data.get("next_cursor"))


def _account_of(instrument: InstrumentRef, account: str | None) -> str:
    if account is not None:
        return account
    return instrument.account if isinstance(instrument, m.Instrument) else ""


def _side(side: Side | str) -> str:
    return Side(side).value


def place(
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
) -> Call[m.OrderAck]:
    resolved = order_type or ("limit" if price is not None else "market")
    body: dict[str, Any] = {
        "account": _account_of(instrument, account),
        "instrument_id": instrument_id(instrument),
        "side": _side(side),
        "order_type": resolved,
        "tif": tif,
        "qty": decimal_text(qty, "qty"),
        "post_only": post_only,
        "reduce_only": reduce_only,
    }
    if price is not None:
        body["price"] = decimal_text(price, "price")
    if external_ref is not None:
        body["external_ref"] = external_ref
    return Call("POST", "/orders", _one(m.OrderAck), body=body, idempotency_key=idempotency_key)


def cancel(order_id: str, idempotency_key: str | None = None) -> Call[m.CancelAck]:
    return Call("DELETE", f"/orders/{order_id}", _one(m.CancelAck), idempotency_key=idempotency_key)


def amend(
    order_id: str,
    *,
    price: DecimalLike | None = None,
    qty: DecimalLike | None = None,
    idempotency_key: str | None = None,
) -> Call[m.OrderAck]:
    body: dict[str, str] = {}
    if price is not None:
        body["price"] = decimal_text(price, "price")
    if qty is not None:
        body["qty"] = decimal_text(qty, "qty")
    return Call("POST", f"/orders/{order_id}/amend", _one(m.OrderAck), body=body, idempotency_key=idempotency_key)


def cancel_all(
    *, account: str | None = None, instrument: InstrumentRef | None = None, idempotency_key: str | None = None
) -> Call[m.CancelAll]:
    body: dict[str, str] = {}
    if account is not None:
        body["account"] = account
    if instrument is not None:
        body["instrument_id"] = instrument_id(instrument)
    return Call("POST", "/orders/cancel-all", _one(m.CancelAll), body=body, idempotency_key=idempotency_key)


def order(order_id: str) -> Call[m.Order]:
    return Call("GET", f"/orders/{order_id}", _one(m.Order))


def order_events(order_id: str) -> Call[list[m.OrderEvent]]:
    return Call("GET", f"/orders/{order_id}/events", _items(m.OrderEvent))


def orders(
    *,
    open: bool | None = None,
    account: str | None = None,
    instrument: InstrumentRef | None = None,
    since: int | None = None,
    cursor: str | None = None,
    limit: int | None = None,
) -> Call[m.Page[m.Order]]:
    query = {
        "open": open,
        "account": account,
        "instrument_id": instrument_id(instrument) if instrument is not None else None,
        "since": since,
        "cursor": cursor,
        "limit": limit,
    }
    return Call("GET", "/orders", _page(m.Order), query=query)


def fills(
    *,
    account: str | None = None,
    instrument: InstrumentRef | None = None,
    since: int | None = None,
    cursor: str | None = None,
    limit: int | None = None,
) -> Call[m.Page[m.Fill]]:
    query = {
        "account": account,
        "instrument_id": instrument_id(instrument) if instrument is not None else None,
        "since": since,
        "cursor": cursor,
        "limit": limit,
    }
    return Call("GET", "/fills", _page(m.Fill), query=query)


def positions(*, account: str | None = None, instrument: InstrumentRef | None = None) -> Call[list[m.Position]]:
    query = {"account": account, "instrument_id": instrument_id(instrument) if instrument is not None else None}
    return Call("GET", "/positions", _items(m.Position), query=query)


def balances(*, account: str | None = None) -> Call[list[m.AccountBalances]]:
    return Call("GET", "/balances", _items(m.AccountBalances), query={"account": account})


def accounts() -> Call[list[m.Account]]:
    return Call("GET", "/accounts", _items(m.Account))


def instrument(instrument_ref: str) -> Call[m.Instrument]:
    return Call("GET", f"/instruments/{instrument_ref}", _one(m.Instrument))


def instruments(
    *,
    account: str | None = None,
    kind: str | None = None,
    cursor: str | None = None,
    limit: int | None = None,
) -> Call[m.Page[m.Instrument]]:
    query = {"account": account, "kind": kind, "cursor": cursor, "limit": limit}
    return Call("GET", "/instruments", _page(m.Instrument), query=query)


def markets(query: str | None = None, *, limit: int | None = None) -> Call[list[m.Market]]:
    return Call("GET", "/markets", _items(m.Market), query={"q": query, "limit": limit})


def option_underlyings() -> Call[list[m.OptionUnderlying]]:
    return Call("GET", "/options/underlyings", lambda rows: [build(m.OptionUnderlying, row) for row in rows])


def option_chain(underlying: str, expiry: str, *, venue: str | None = None) -> Call[m.OptionChain]:
    query = {"underlying": underlying, "expiry": expiry, "venue": venue}
    return Call("GET", "/options/chain", _one(m.OptionChain), query=query)


def option_surface(underlying: str, *, venue: str | None = None) -> Call[dict[str, Any]]:
    return Call("GET", "/options/surface", lambda data: data, query={"underlying": underlying, "venue": venue})


def whoami() -> Call[m.Whoami]:
    return Call("GET", "/whoami", _one(m.Whoami))


def server_time() -> Call[int]:
    return Call("GET", "/time", lambda data: int(data["server_time_ms"]), signed=False)


def algos() -> Call[list[m.AlgoOrder]]:
    return Call("GET", "/algos", _items(m.AlgoOrder))


def algo(algo_id: str) -> Call[m.AlgoOrder]:
    return Call("GET", f"/algos/{algo_id}", _one(m.AlgoOrder))


def algo_action(algo_id: str, action: str, idempotency_key: str | None = None) -> Call[m.AlgoOrder]:
    return Call("POST", f"/algos/{algo_id}/{action}", _one(m.AlgoOrder), idempotency_key=idempotency_key)


def start_algo(
    kind: str,
    symbol: str,
    side: Side | str,
    *,
    qty: DecimalLike | None = None,
    notional: DecimalLike | None = None,
    limit: DecimalLike | None = None,
    market: str = "perp",
    horizon_s: int | None = None,
    slices: int | None = None,
    jitter_bp: int | None = None,
    catchup_pct: int | None = None,
    profile: str | None = None,
    pov_bp: int | None = None,
    max_child_notional: DecimalLike | None = None,
    external_ref: str | None = None,
    idempotency_key: str | None = None,
) -> Call[m.AlgoOrder]:
    if (qty is None) == (notional is None):
        raise ValueError("give exactly one of qty and notional")
    body: dict[str, Any] = {"symbol": symbol, "market": market, "side": _side(side), "algo": kind}
    for name, value in (
        ("qty", qty),
        ("notional", notional),
        ("limit", limit),
        ("max_child_notional", max_child_notional),
    ):
        if value is not None:
            body[name] = decimal_text(value, name)
    for name, whole in (
        ("horizon_s", horizon_s),
        ("slices", slices),
        ("jitter_bp", jitter_bp),
        ("catchup_pct", catchup_pct),
        ("pov_bp", pov_bp),
    ):
        if whole is not None:
            body[name] = whole
    if profile is not None:
        body["profile"] = profile
    if external_ref is not None:
        body["external_ref"] = external_ref
    return Call("POST", "/algos", _one(m.AlgoOrder), body=body, idempotency_key=idempotency_key)
