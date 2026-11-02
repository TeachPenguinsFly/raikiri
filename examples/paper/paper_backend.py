"""An in-memory stand-in for admind, for trying the client API with no venue and no money.

It answers the routes apid calls, with a random-walk market, instant matching against that market, and parent orders
that slice over real time. Prices are made up. Nothing here leaves the machine.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import random
import time
from dataclasses import dataclass, field
from decimal import ROUND_HALF_UP, Decimal
from typing import Any
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import ServerConnection, serve

NANOS = 1_000_000_000
TAKER_FEE = Decimal("0.0004")
DUPLICATE_WINDOW_NS = 50_000_000
REFUSED_DUPLICATE = -2  # libs/domain/include/raikiri/domain/refusal_codes.hpp
EXPIRY_DATE = "20261127"
EXPIRY_NS = 1_795_737_600 * NANOS


def text(value: Decimal) -> str:
    return format(value.normalize(), "f")


def now_ns() -> int:
    return time.time_ns()


@dataclass
class Listing:
    instrument_id: str
    account: str
    venue: str
    symbol: str
    mid: Decimal
    market: str = "perp"
    tick: Decimal = Decimal("0.1")
    lot: Decimal = Decimal("0.001")
    strike: Decimal | None = None
    put_call: str | None = None

    def snap(self, price: Decimal) -> Decimal:
        return (price / self.tick).quantize(Decimal(1), ROUND_HALF_UP) * self.tick

    @property
    def half_spread(self) -> Decimal:
        return self.tick * 2

    @property
    def bid(self) -> Decimal:
        return self.snap(self.mid - self.half_spread)

    @property
    def ask(self) -> Decimal:
        return self.snap(self.mid + self.half_spread)

    def row(self) -> dict[str, Any]:
        row = {
            "instrument_id": self.instrument_id,
            "account": self.account,
            "venue": self.venue,
            "market": self.market,
            "exchange_symbol": self.symbol.replace("/", ""),
            "local_symbol": self.symbol,
            "tick_size": text(self.tick),
            "lot_size": text(self.lot),
            "min_notional": "1",
            "contract_value": "1",
            "contract_kind": "linear",
            "asset_class": "crypto",
            "asset_key": "crypto:" + self.symbol,
        }
        if self.strike is not None:
            row.update(expiry=str(EXPIRY_NS), strike=text(self.strike), put_call=self.put_call)
        return row


@dataclass
class Order:
    id: str
    listing: Listing
    side: str
    order_type: str
    tif: str
    price: Decimal | None
    qty: Decimal
    strategy: int
    post_only: bool = False
    reduce_only: bool = False
    book: str = "manual"
    external_ref: str | None = None
    reject_code: int | None = None
    leaves: Decimal = Decimal(0)
    status: str = "submitted"
    ts_created: int = 0
    ts_updated: int = 0
    events: list[dict[str, Any]] = field(default_factory=list)

    def row(self) -> dict[str, Any]:
        row = {
            "client_order_id": self.id,
            "instrument_id": self.listing.instrument_id,
            "strategy_id": self.strategy,
            "side": self.side,
            "order_type": self.order_type,
            "tif": self.tif,
            "post_only": self.post_only,
            "reduce_only": self.reduce_only,
            "qty": text(self.qty),
            "leaves": text(self.leaves),
            "status": self.status,
            "ts_created": str(self.ts_created),
            "ts_updated": str(self.ts_updated),
        }
        row["book"] = self.book
        if self.external_ref:
            row["external_ref"] = self.external_ref
        if self.reject_code is not None:
            row["reject_code"] = self.reject_code
        if self.price is not None:
            row["price"] = text(self.price)
        return row


@dataclass
class Parent:
    id: str
    symbol: str
    market: str
    side: str
    algo: str
    profile: str
    qty: Decimal
    limit: Decimal | None
    horizon_s: int
    slices: int
    pov_bp: int
    created: int
    status: str = "working"
    reason: str | None = None
    arrival: Decimal | None = None
    filled: Decimal = Decimal(0)
    cost: Decimal = Decimal(0)
    fees: Decimal = Decimal(0)
    fired: int = 0
    mid_sum: Decimal = Decimal(0)
    started: int = 0
    ended: int | None = None
    children: list[dict[str, Any]] = field(default_factory=list)
    wake: asyncio.Event = field(default_factory=asyncio.Event)
    book: str = "manual"
    external_ref: str | None = None

    def row(self, with_children: bool = True) -> dict[str, Any]:
        row: dict[str, Any] = {
            "id": self.id,
            "symbol": self.symbol,
            "market": self.market,
            "side": self.side,
            "algo": self.algo,
            "profile": self.profile,
            "status": self.status,
            "qty": text(self.qty),
            "filled": text(self.filled),
            "working": "0",
            "fees": text(self.fees),
            "horizon_s": self.horizon_s,
            "slices": self.slices,
            "slices_fired": self.fired,
            "pov_bp": self.pov_bp,
            "created": str(self.created),
            "child_count": len(self.children),
            "started": str(self.started),
            "book": self.book,
        }
        if self.external_ref:
            row["external_ref"] = self.external_ref
        if self.reason is not None:
            row["reason"] = self.reason
        if self.limit is not None:
            row["limit"] = text(self.limit)
        if self.arrival is not None:
            row["arrival"] = text(self.arrival)
        if self.filled > 0:
            avg = self.cost / self.filled
            sign = 1 if self.side == "buy" else -1
            row["avg_price"] = text(avg.quantize(Decimal("0.000001")))
            vwap = self.mid_sum / max(self.fired, 1)
            row["market_vwap"] = text(vwap.quantize(Decimal("0.000001")))
            if self.arrival:
                bp = (avg - self.arrival) / self.arrival * 10000 * sign
                row["slippage_arrival_bp"] = text(bp.quantize(Decimal("0.0001")))
            bp_vwap = (avg - vwap) / vwap * 10000 * sign
            row["slippage_vwap_bp"] = text(bp_vwap.quantize(Decimal("0.0001")))
        if self.ended is not None:
            row["ended"] = str(self.ended)
        if with_children:
            row["children"] = self.children
        return row


class Connection:
    def __init__(self, socket: ServerConnection) -> None:
        self.socket = socket
        self.subs: dict[str, dict[str, Any]] = {}
        self.event_id: str | None = None
        self.fill_id: str | None = None
        self.seq = 0


class Paper:
    def __init__(self, seed: int | None = None) -> None:
        self.rng = random.Random(seed)
        self.listings: dict[str, Listing] = {}
        for listing in (
            Listing("1001", "binance", "binance", "BTC/USDT", Decimal("68000")),
            Listing("1002", "binance", "binance", "ETH/USDT", Decimal("2600"), tick=Decimal("0.01")),
            Listing("2001", "okx", "okx", "BTC/USDT", Decimal("68005")),
            Listing("2002", "okx", "okx", "ETH/USDT", Decimal("2601"), tick=Decimal("0.01")),
        ):
            self.listings[listing.instrument_id] = listing
        strike = Decimal("64000")
        number = 3001
        while strike <= Decimal("72000"):
            for put_call, code in (("call", "C"), ("put", "P")):
                symbol = f"BTC-{EXPIRY_DATE}-{strike}-{code}"
                premium = max(Decimal("68000") - strike if code == "C" else strike - Decimal("68000"), Decimal(0))
                listing = Listing(
                    str(number),
                    "deribit",
                    "deribit",
                    symbol,
                    premium + Decimal(900),
                    "option",
                    Decimal("0.5"),
                    Decimal("0.1"),
                    strike,
                    put_call,
                )
                self.listings[listing.instrument_id] = listing
                number += 1
            strike += Decimal(2000)
        self.orders: dict[str, Order] = {}
        self.order_log: list[str] = []
        self.fills: list[dict[str, Any]] = []
        self.parents: dict[str, Parent] = {}
        self.keys: dict[str, str] = {}
        self.next_order = 7_000_000
        self.next_parent = 1
        self.next_exec = 1
        self.connections: set[Connection] = set()
        self.books: dict[str, int] = {}
        self.last_shape: dict[tuple[Any, ...], int] = {}
        self.balance = {"binance": Decimal(100000), "okx": Decimal(100000)}

    def mint(self) -> str:
        self.next_order += 1
        return str(self.next_order)

    def record(self, order: Order, event_type: str, **extra: Any) -> None:
        order.ts_updated = now_ns()
        row = {
            "client_order_id": order.id,
            "instrument_id": order.listing.instrument_id,
            "event_type": event_type,
            "leaves": text(order.leaves),
            "ts_event": str(order.ts_updated),
            **extra,
        }
        order.events.append(row)
        for connection in list(self.connections):
            if connection.event_id is not None:
                connection.seq += 1
                self.send(
                    connection,
                    {"type": "event", "id": connection.event_id, "seq": str(connection.seq), "order_event": row},
                )

    def send(self, connection: Connection, frame: dict[str, Any]) -> None:
        asyncio.ensure_future(self.deliver(connection, frame))

    async def deliver(self, connection: Connection, frame: dict[str, Any]) -> None:
        try:
            await connection.socket.send(json.dumps(frame))
        except Exception:  # noqa: BLE001 - a closed socket drops out of the set on its own
            pass

    def fill(self, order: Order, price: Decimal, qty: Decimal, liquidity: str) -> None:
        fee = (price * qty * TAKER_FEE).quantize(Decimal("0.00000001"))
        exec_id = f"px-{self.next_exec}"
        self.next_exec += 1
        order.leaves -= qty
        order.status = "filled" if order.leaves == 0 else "partially_filled"
        self.record(order, "filled", price=text(price), qty=text(qty), fee=text(fee), exec_id=exec_id)
        row = {
            "venue": order.listing.venue,
            "exec_id": exec_id,
            "client_order_id": order.id,
            "exchange_order_id": "x" + order.id,
            "instrument_id": order.listing.instrument_id,
            "side": order.side,
            "price": text(price),
            "qty": text(qty),
            "fee": text(fee),
            "liquidity": liquidity,
            "ts_event": str(order.ts_updated),
        }
        self.fills.append(row)
        for connection in list(self.connections):
            if connection.fill_id is not None:
                connection.seq += 1
                self.send(
                    connection, {"type": "event", "id": connection.fill_id, "seq": str(connection.seq), "fill": row}
                )

    def try_match(self, order: Order) -> None:
        listing = order.listing
        if order.leaves == 0 or order.status in ("filled", "canceled", "rejected", "expired"):
            return
        if order.order_type == "market":
            self.fill(order, listing.ask if order.side == "buy" else listing.bid, order.leaves, "taker")
            return
        assert order.price is not None
        if order.side == "buy" and order.price >= listing.ask:
            self.fill(order, listing.ask, order.leaves, "taker" if order.ts_updated == order.ts_created else "maker")
        elif order.side == "sell" and order.price <= listing.bid:
            self.fill(order, listing.bid, order.leaves, "taker" if order.ts_updated == order.ts_created else "maker")
        elif order.tif != "gtc":
            order.status = "expired"
            self.record(order, "expired")

    def place(self, body: dict[str, Any], key: str) -> tuple[int, dict[str, Any]]:
        if key in self.keys:
            return 202, {"client_order_id": self.keys[key], "status": "queued"}
        listing = self.listings.get(str(body.get("instrument_id")))
        if listing is None:
            return problem(422, "refused", "no attached gateway lists this instrument")
        book = str(body.get("book", "manual"))
        if book != "manual" and book not in self.books:
            return problem(404, "bad book", "no such book")
        qty = Decimal(str(body.get("qty")))
        if qty <= 0 or qty % listing.lot != 0:
            return problem(422, "refused", "quantity is not a multiple of the instrument's lot")
        price = None
        kind = body.get("order_type")
        if kind == "limit":
            if body.get("price") is None:
                return problem(422, "refused", "a limit order needs a price")
            price = Decimal(str(body["price"]))
            if price % listing.tick != 0:
                return problem(422, "refused", "price is not a multiple of the instrument's tick")
        elif body.get("price") is not None:
            return problem(422, "refused", "a market order must not carry a price")
        order = self.new_order(listing, str(body.get("side")), str(kind), str(body.get("tif", "gtc")), price, qty, 1)
        order.post_only = bool(body.get("post_only"))
        order.reduce_only = bool(body.get("reduce_only"))
        order.book = book
        order.external_ref = body.get("external_ref")
        self.keys[key] = order.id
        # as strategyd's window: the same order again at once is refused after the 202, and the earlier one stands
        shape = (book, listing.instrument_id, order.side, price, qty)
        stamp = now_ns()
        earlier = self.last_shape.get(shape, 0)
        self.last_shape[shape] = stamp
        if kind == "limit" and stamp - earlier < DUPLICATE_WINDOW_NS:
            order.status = "denied"
            order.leaves = Decimal(0)
            order.reject_code = REFUSED_DUPLICATE
            self.record(order, "denied")
            return 202, {"client_order_id": order.id, "status": "queued"}
        self.submit(order)
        return 202, {"client_order_id": order.id, "status": "queued"}

    def new_order(
        self, listing: Listing, side: str, kind: str, tif: str, price: Decimal | None, qty: Decimal, strategy: int
    ) -> Order:
        stamp = now_ns()
        order = Order(
            self.mint(), listing, side, kind, tif, price, qty, strategy, leaves=qty, ts_created=stamp, ts_updated=stamp
        )
        self.orders[order.id] = order
        self.order_log.append(order.id)
        return order

    def submit(self, order: Order) -> None:
        self.record(order, "submitted")
        order.status = "accepted"
        self.record(order, "accepted")
        self.try_match(order)

    def cancel(self, order_id: str) -> tuple[int, dict[str, Any]]:
        order = self.orders.get(order_id)
        if order is None:
            return problem(404, "refused", "no such order on this desk")
        if order.status in ("filled", "canceled", "rejected", "expired"):
            return problem(409, "refused", "the order is closed")
        order.status = "canceled"
        self.record(order, "canceled")
        return 202, {"client_order_id": order_id, "status": "queued"}

    def page(self, rows: list[dict[str, Any]], query: dict[str, str]) -> dict[str, Any]:
        start = int(query.get("cursor", 0))
        limit = int(query.get("limit", 200))
        end = start + limit
        out: dict[str, Any] = {"items": rows[start:end]}
        if end < len(rows):
            out["next_cursor"] = str(end)
        return out

    def start_parent(self, body: dict[str, Any]) -> tuple[int, dict[str, Any]]:
        symbol = str(body.get("symbol"))
        venues = [
            row for row in self.listings.values() if row.symbol == symbol and row.market == body.get("market", "perp")
        ]
        if not venues:
            return problem(422, "refused", f"nothing lists {symbol}")
        side = str(body.get("side"))
        arrival = sum((row.mid for row in venues), Decimal(0)) / len(venues)
        if body.get("qty") is not None:
            qty = Decimal(str(body["qty"]))
        elif body.get("notional") is not None:
            qty = (Decimal(str(body["notional"])) / arrival).quantize(venues[0].lot)
        else:
            return problem(400, "not a parent order", "give qty or notional")
        kind = str(body.get("algo", "twap"))
        parent = Parent(
            str(self.next_parent),
            symbol,
            str(body.get("market", "perp")),
            side,
            kind,
            str(body.get("profile", "flat")),
            qty,
            Decimal(str(body["limit"])) if body.get("limit") else None,
            int(body.get("horizon_s", 0)),
            int(body.get("slices", 1)),
            int(body.get("pov_bp", 0)),
            now_ns(),
            arrival=arrival,
            started=now_ns(),
        )
        self.next_parent += 1
        parent.book = str(body.get("book", "manual"))
        parent.external_ref = body.get("external_ref")
        self.parents[parent.id] = parent
        asyncio.ensure_future(self.run_parent(parent, venues))
        return 201, {"id": parent.id}

    async def run_parent(self, parent: Parent, venues: list[Listing]) -> None:
        slices = 1 if parent.algo == "sor" else max(parent.slices, 1)
        if parent.algo == "pov":
            slices = max(parent.horizon_s, 5)
        interval = parent.horizon_s / slices if parent.horizon_s else 0.05
        for index in range(slices):
            while parent.status == "paused":
                await asyncio.sleep(0.02)
            if parent.status != "working":
                return
            remaining = parent.qty - parent.filled
            lot = venues[0].lot
            size = (remaining / (slices - index)).quantize(lot) if index < slices - 1 else remaining
            if parent.algo == "sor":
                # split across venues, best first, as a router would when one book cannot take it all
                ranked = sorted(venues, key=lambda row: row.ask if parent.side == "buy" else -row.bid)
                shares = [(ranked[0], (remaining * 2 / 3).quantize(lot))] if len(ranked) > 1 else []
                shares.append((ranked[-1], remaining - sum((share for _, share in shares), Decimal(0))))
            else:
                best = (
                    min(venues, key=lambda row: row.ask)
                    if parent.side == "buy"
                    else max(venues, key=lambda row: row.bid)
                )
                shares = [(best, size)]
            parent.fired += 1
            parent.mid_sum += sum((row.mid for row in venues), Decimal(0)) / len(venues)
            for listing, share in shares:
                if share > 0:
                    self.send_child(parent, listing, share)
            if index < slices - 1:
                await asyncio.sleep(interval)
        parent.status = "done"
        parent.ended = now_ns()

    def send_child(self, parent: Parent, listing: Listing, qty: Decimal) -> None:
        touch = listing.ask if parent.side == "buy" else listing.bid
        if parent.limit is not None and (
            (parent.side == "buy" and touch > parent.limit) or (parent.side == "sell" and touch < parent.limit)
        ):
            return
        order = self.new_order(listing, parent.side, "limit", "ioc", touch, qty, 2)
        child = {
            "client_order_id": order.id,
            "instrument_id": listing.instrument_id,
            "venue": listing.venue,
            "qty": text(qty),
            "limit": text(touch),
            "filled": text(qty),
            "avg_price": text(touch),
            "fees": text((touch * qty * TAKER_FEE).quantize(Decimal("0.00000001"))),
            "sent": str(order.ts_created),
            "acked": str(order.ts_created),
            "closed": str(order.ts_created),
            "end": "filled",
            "open": False,
        }
        parent.children.append(child)
        parent.filled += qty
        parent.cost += touch * qty
        parent.fees += Decimal(child["fees"])

        # the fill reaches the stream after the child is visible on the parent, as joe's children do
        async def later() -> None:
            await asyncio.sleep(0.01)
            self.submit(order)

        asyncio.ensure_future(later())

    def parent_action(self, parent_id: str, action: str) -> tuple[int, dict[str, Any]]:
        parent = self.parents.get(parent_id)
        if parent is None:
            return problem(404, "no such parent", parent_id)
        if parent.status in ("done", "cancelled", "failed"):
            return problem(409, "not in a state for that", parent.status)
        if action == "pause":
            parent.status = "paused"
        elif action == "resume":
            parent.status = "working"
        elif action == "cancel":
            parent.status = "cancelled"
            parent.ended = now_ns()
        return 200, parent.row()

    def chain(self, query: dict[str, str]) -> tuple[int, dict[str, Any]]:
        if query.get("underlying") != "BTC":
            return problem(404, "no options on that underlying", query.get("underlying", ""))
        if query.get("expiry") != EXPIRY_DATE:
            return problem(404, "no such expiry", query.get("expiry", ""))
        forward = self.listings["1001"].mid
        strikes: dict[Decimal, dict[str, Any]] = {}
        for listing in self.listings.values():
            if listing.strike is None:
                continue
            moneyness = (listing.strike - forward) / forward
            delta = (Decimal("0.5") - moneyness * 4).quantize(Decimal("0.0001"))
            if listing.put_call == "put":
                delta -= 1
            iv = (Decimal("0.55") + abs(moneyness)).quantize(Decimal("0.0001"))
            quote = {
                "venue": listing.venue,
                "instrument_id": listing.instrument_id,
                "symbol": listing.symbol,
                "premium_unit": "usd",
                "bid": text(listing.bid),
                "ask": text(listing.ask),
                "mark": text(listing.snap(listing.mid)),
                "bid_usd": text(listing.bid),
                "ask_usd": text(listing.ask),
                "mark_usd": text(listing.snap(listing.mid)),
                "mark_iv": text(iv),
                "bid_iv": text(iv - Decimal("0.01")),
                "ask_iv": text(iv + Decimal("0.01")),
                "iv_source": "venue",
                "delta": text(delta),
                "gamma": "0.00002",
                "vega": "45.5",
                "theta": "-60.25",
                "greeks_source": "venue",
                "greeks_unit": "usd",
                "ts_event": str(now_ns()),
            }
            best = {
                "venues": [quote],
                "best_bid": {"venue": listing.venue, "usd": quote["bid_usd"], "iv": quote["bid_iv"]},
                "best_ask": {"venue": listing.venue, "usd": quote["ask_usd"], "iv": quote["ask_iv"]},
            }
            strikes.setdefault(listing.strike, {"strike": text(listing.strike)})[listing.put_call or "call"] = best
        rows = [strikes[key] for key in sorted(strikes)]
        return 200, {
            "underlying": "BTC",
            "expiry": str(EXPIRY_NS),
            "date": EXPIRY_DATE,
            "as_of": str(now_ns()),
            "years": "0.07",
            "seconds_to_expiry": max((EXPIRY_NS - now_ns()) // NANOS, 0),
            "forward": text(forward),
            "forward_source": "parity",
            "atm_strike": text(min(strikes, key=lambda strike: abs(strike - forward))),
            "venues": [{"venue": "deribit", "index": text(forward)}],
            "rows": rows,
        }

    def route(self, method: str, target: str, body: bytes, key: str) -> tuple[int, Any]:
        parts = urlsplit(target)
        path = parts.path
        query = {name: values[0] for name, values in parse_qs(parts.query).items()}
        data = json.loads(body) if body else {}
        if path == "/v1/books" and method == "POST":
            name = str(data.get("name", ""))
            if not name.startswith("client:") or len(name) == len("client:"):
                return problem(422, "not a client's book name", name)
            fresh = name not in self.books
            book_id = self.books.setdefault(name, 1000 + len(self.books))
            return (201 if fresh else 200), {"id": book_id, "kind": "client", "name": name}
        if path == "/v1/stream/token":
            return 200, {"token": "paper", "expires_at": str(now_ns() + 60 * NANOS)}
        if path == "/v1/accounts":
            return 200, [
                {"account": name, "venue": name, "market": "perp", "gateway": {"alive": True}}
                for name in ("binance", "okx", "deribit")
            ]
        if path == "/v1/balances":
            return 200, [
                {
                    "account": name,
                    "venue": name,
                    "reported_at": str(now_ns()),
                    "stale": False,
                    "wallet": "unified",
                    "assets": [{"asset": "USDT", "total": text(value), "available": text(value)}],
                }
                for name, value in self.balance.items()
            ]
        if path == "/v1/instruments":
            rows = [row.row() for row in self.listings.values()]
            if query.get("account"):
                rows = [row for row in rows if row["account"] == query["account"]]
            if query.get("kind"):
                rows = [row for row in rows if row["market"] == query["kind"]]
            return 200, self.page(rows, query)
        if path.startswith("/v1/instruments/"):
            listing = self.listings.get(path.rsplit("/", 1)[1])
            return (200, listing.row()) if listing else problem(404, "no such instrument", path)
        if path == "/v1/markets":
            groups: dict[str, dict[str, Any]] = {}
            for listing in self.listings.values():
                if query.get("q") and query["q"].upper() not in listing.symbol.upper():
                    continue
                group = groups.setdefault(
                    listing.symbol, {"key": "crypto:" + listing.symbol, "symbol": listing.symbol, "listings": []}
                )
                group["listings"].append(
                    {
                        key: listing.row()[key]
                        for key in ("instrument_id", "account", "venue", "market", "exchange_symbol", "local_symbol")
                    }
                )
            return 200, list(groups.values())
        if path == "/v1/orders" and method == "POST":
            if not key:
                return problem(400, "Idempotency-Key required")
            return self.place(data, key)
        if path == "/v1/orders":
            rows = [self.orders[oid].row() for oid in reversed(self.order_log)]
            if query.get("open") == "true":
                rows = [row for row in rows if row["status"] not in ("filled", "canceled", "rejected", "expired")]
            if query.get("instrument_id"):
                rows = [row for row in rows if row["instrument_id"] == query["instrument_id"]]
            if query.get("book"):
                rows = [row for row in rows if row["book"] == query["book"]]
            return 200, self.page(rows, query)
        if path.startswith("/v1/orders/"):
            tail = path[len("/v1/orders/") :]
            if method == "DELETE":
                return self.cancel(tail)
            if tail.endswith("/events"):
                order = self.orders.get(tail[: -len("/events")])
                return (200, order.events) if order else problem(404, "no such order")
            order = self.orders.get(tail)
            return (200, order.row()) if order else problem(404, "no such order", tail)
        if path == "/v1/fills":
            since = int(query.get("since", 0))
            rows = [row for row in reversed(self.fills) if int(row["ts_event"]) >= since]
            return 200, self.page(rows, query)
        if path == "/v1/joe/parents" and method == "POST":
            return self.start_parent(data)
        if path.startswith("/v1/joe/parents/"):
            tail = path[len("/v1/joe/parents/") :].split("/")
            if len(tail) == 2 and method == "POST":
                return self.parent_action(tail[0], tail[1])
            parent = self.parents.get(tail[0])
            return (200, parent.row()) if parent else problem(404, "no such parent", tail[0])
        if path == "/v1/options/underlyings":
            return 200, [
                {
                    "underlying": "BTC",
                    "venues": [{"venue": "deribit", "options": 10}],
                    "expiries": [{"expiry": str(EXPIRY_NS), "date": EXPIRY_DATE, "strikes": 5, "venues": ["deribit"]}],
                }
            ]
        if path == "/v1/options/chain":
            return self.chain(query)
        return problem(404, "no such path", path)

    def walk(self) -> None:
        for listing in self.listings.values():
            if listing.market == "perp":
                step = listing.mid * Decimal(str(self.rng.gauss(0, 0.00005)))
                listing.mid = listing.snap(listing.mid + step)
        for order in list(self.orders.values()):
            if order.status in ("accepted", "partially_filled") and order.order_type == "limit" and order.tif == "gtc":
                self.try_match(order)

    def frames_for(self, connection: Connection) -> None:
        stamp = str(now_ns())
        for sub_id, sub in connection.subs.items():
            sub["due"] -= 1
            if sub["due"] > 0:
                continue
            sub["due"] = sub["every"]
            if sub.get("quotes"):
                rows = [self.quote(identity) for identity in sub["quotes"]]
                self.send(connection, {"type": "quotes", "id": sub_id, "ts": stamp, "quotes": rows})
            if sub.get("book"):
                self.send(
                    connection,
                    {"type": "book", "id": sub_id, "ts": stamp, "book": self.book(sub["book"], sub.get("levels", 5))},
                )
            if sub.get("greeks"):
                rows = [self.greeks(identity) for identity in sub["greeks"]]
                self.send(connection, {"type": "greeks", "id": sub_id, "ts": stamp, "greeks": rows})
            if sub.get("funding"):
                rows = [
                    {
                        "instrument_id": i,
                        "rate": "0.0001",
                        "predicted_rate": "0.00012",
                        "mark_price": text(self.listings[i].mid),
                        "next_funding_time": str(now_ns() + 3600 * NANOS),
                        "interval_seconds": 28800,
                        "ts_event": stamp,
                    }
                    for i in sub["funding"]
                ]
                self.send(connection, {"type": "funding", "id": sub_id, "ts": stamp, "funding": rows})
            if sub.get("ticker"):
                rows = [
                    {
                        "instrument_id": i,
                        "volume_24h_base": "12345.5",
                        "volume_24h_quote": "839000000",
                        "ts_event": stamp,
                    }
                    for i in sub["ticker"]
                ]
                self.send(connection, {"type": "ticker", "id": sub_id, "ts": stamp, "ticker": rows})

    def quote(self, identity: str) -> dict[str, Any]:
        listing = self.listings[identity]
        return {
            "instrument_id": identity,
            "bid": text(listing.bid),
            "bid_size": "1.5",
            "ask": text(listing.ask),
            "ask_size": "1.25",
            "ts_event": str(now_ns()),
        }

    def book(self, identity: str, levels: int) -> dict[str, Any]:
        listing = self.listings[identity]
        bids = [
            {"price": text(listing.bid - listing.tick * n), "size": text(Decimal("0.5") + Decimal(n))}
            for n in range(levels)
        ]
        asks = [
            {"price": text(listing.ask + listing.tick * n), "size": text(Decimal("0.5") + Decimal(n))}
            for n in range(levels)
        ]
        return {"instrument_id": identity, "bids": bids, "asks": asks, "ts_event": str(now_ns())}

    def greeks(self, identity: str) -> dict[str, Any]:
        listing = self.listings[identity]
        forward = text(self.listings["1001"].mid)
        return {
            "instrument_id": identity,
            "delta": "0.5",
            "gamma": "0.00002",
            "vega": "45.5",
            "theta": "-60.25",
            "rho": "0.1",
            "mark_iv": "0.55",
            "bid_iv": "0.54",
            "ask_iv": "0.56",
            "underlying_price": forward,
            "mark_price": text(listing.mid),
            "index_price": forward,
            "ts_event": str(now_ns()),
        }

    async def rest(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                head = await reader.readuntil(b"\r\n\r\n")
                lines = head.decode().split("\r\n")
                method, target, _ = lines[0].split(" ", 2)
                headers = {
                    name.lower(): value.strip()
                    for name, _, value in (line.partition(":") for line in lines[1:] if line)
                }
                body = await reader.readexactly(int(headers.get("content-length", 0)))
                status, data = self.route(method, target, body, headers.get("idempotency-key", ""))
                payload = json.dumps(data).encode()
                kind = "application/problem+json" if status >= 400 else "application/json"
                writer.write(
                    f"HTTP/1.1 {status} X\r\nContent-Type: {kind}\r\nContent-Length: {len(payload)}\r\n\r\n".encode()
                    + payload
                )
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()

    async def stream(self, socket: ServerConnection) -> None:
        request = socket.request
        if request is None or "token=" not in request.path:
            await socket.close(1008)
            return
        connection = Connection(socket)
        self.connections.add(connection)
        try:
            async for message in socket:
                data = json.loads(message)
                sub_id = str(data.get("id", ""))
                if data.get("op") == "unsubscribe":
                    connection.subs.pop(sub_id, None)
                    await socket.send(json.dumps({"type": "ack", "id": sub_id}))
                    continue
                if data.get("events"):
                    if "orders" in data["events"]:
                        connection.event_id = sub_id
                    if "fills" in data["events"]:
                        connection.fill_id = sub_id
                    await socket.send(json.dumps({"type": "ack", "id": sub_id}))
                    continue
                hz = float(data.get("hz", 10))
                sub = {
                    name: data[name]
                    for name in ("quotes", "book", "levels", "greeks", "funding", "ticker")
                    if name in data
                }
                sub["every"] = max(int(20 / max(hz, 0.1)), 1)
                sub["due"] = 0
                connection.subs[sub_id] = sub
                await socket.send(json.dumps({"type": "ack", "id": sub_id, "hz": hz}))
        finally:
            self.connections.discard(connection)

    async def run(self, host: str, rest_port: int, stream_port: int) -> None:
        rest = await asyncio.start_server(self.rest, host, rest_port)
        async with serve(self.stream, host, stream_port), rest:
            while True:
                await asyncio.sleep(0.05)
                self.walk()
                for connection in list(self.connections):
                    self.frames_for(connection)


def problem(status: int, title: str, detail: str = "") -> tuple[int, dict[str, Any]]:
    return status, {"title": title, "status": status, "detail": detail}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18787)
    parser.add_argument("--stream-port", type=int, default=18788)
    parser.add_argument("--seed", type=int)
    args = parser.parse_args()
    # a client that drops its socket is ordinary here
    logging.getLogger("websockets.server").setLevel(logging.CRITICAL)
    asyncio.run(Paper(args.seed).run(args.host, args.port, args.stream_port))


if __name__ == "__main__":
    main()
