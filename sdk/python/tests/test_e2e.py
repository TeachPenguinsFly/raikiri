"""The whole path: this package, a real apid, and the paper backend in place of admind."""

from __future__ import annotations

import hashlib
import hmac
import importlib.util
import sys
import time
from collections.abc import Iterator
from decimal import Decimal
from pathlib import Path

import httpx
import pytest

from raikiri import (
    AlgoOrder,
    AsyncClient,
    AuthenticationError,
    Book,
    Client,
    Fill,
    NotFound,
    OrderEvent,
    OrderRejected,
    OutOfScope,
    PermissionDenied,
    Position,
    Quote,
    RateLimited,
)

ROOT = Path(__file__).resolve().parents[3]
PAPER = ROOT / "examples" / "paper" / "paper.py"

spec = importlib.util.spec_from_file_location("raikiri_paper", PAPER)
assert spec is not None and spec.loader is not None
paper = importlib.util.module_from_spec(spec)
sys.modules["raikiri_paper"] = paper
spec.loader.exec_module(paper)

pytestmark = pytest.mark.skipif(paper.find_apid() is None, reason="no apid binary; build the apid target")


@pytest.fixture(scope="module")
def deployed() -> Iterator[object]:
    secret = "e2e-" + "0123456789abcdef" * 3
    keys = [
        {
            "id": "full",
            "client_id": "acme",
            "secret": f"env:{paper.SECRET_ENV}",
            "permissions": ["read", "trade", "algo"],
            "accounts": ["*"],
            "symbols": ["*"],
            "rate": {"requests_per_second": 500, "burst": 1000, "mutations_per_second": 200, "mutation_burst": 200},
        },
        {
            "id": "other",
            "client_id": "globex",
            "secret": f"env:{paper.SECRET_ENV}",
            "permissions": ["read", "trade", "algo"],
            "accounts": ["*"],
            "symbols": ["*"],
            "rate": {"requests_per_second": 500, "burst": 1000, "mutations_per_second": 200, "mutation_burst": 200},
        },
        {
            "id": "reader",
            "client_id": "acme",
            "secret": f"env:{paper.SECRET_ENV}",
            "permissions": ["read"],
            "accounts": ["*"],
            "symbols": ["*"],
        },
        {
            "id": "narrow",
            "client_id": "acme",
            "secret": f"env:{paper.SECRET_ENV}",
            "permissions": ["read", "trade", "algo"],
            "accounts": ["binance"],
            "symbols": ["BTC/*"],
        },
        {
            "id": "slow",
            "client_id": "acme",
            "secret": f"env:{paper.SECRET_ENV}",
            "permissions": ["read"],
            "accounts": ["*"],
            "symbols": ["*"],
            "rate": {"requests_per_second": 1, "burst": 2, "mutations_per_second": 1, "mutation_burst": 1},
        },
    ]
    with paper.start(keys=keys, secret=secret, extra_backend=["--seed", "7"]) as deployment:
        yield deployment


def client_of(deployed, key: str = "full", **options) -> Client:
    return Client(deployed.url, key=key, secret=deployed.secret, **options)


def btc(api: Client):
    return api.market.find("BTC/USDT", account="binance")


def test_install_to_order_in_five_lines(deployed):
    api = Client(deployed.url, key="full", secret=deployed.secret)
    instrument = api.market.find("BTC/USDT", account="binance")
    order = api.orders.limit(instrument, "buy", "0.001", "1000")
    assert api.orders.get(order.id).status == "accepted"
    api.orders.cancel(order.id)
    assert api.orders.wait(order.id).status == "canceled"
    api.close()


def test_a_crossing_order_fills_and_the_position_follows(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        before = {row.instrument_id: row.qty for row in api.account.positions()}.get(
            instrument.instrument_id, Decimal(0)
        )
        order = api.orders.market(instrument, "buy", "0.002")
        filled = api.orders.wait(order.id)
        assert filled.status == "filled"
        assert filled.client_id == "acme"
        assert filled.account == "binance"
        assert filled.filled == Decimal("0.002")
        deadline = time.monotonic() + 5
        fills = []
        while time.monotonic() < deadline and not fills:
            fills = [row for row in api.account.iter_fills() if row.client_order_id == order.id]
            time.sleep(0.05)
        assert fills and fills[0].client_id == "acme"
        positions = {row.instrument_id: row for row in api.account.positions()}
        assert positions[instrument.instrument_id].qty == before + Decimal("0.002")
        assert positions[instrument.instrument_id].avg_entry_price is not None


def test_the_same_idempotency_key_places_once(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        one = api.orders.limit(instrument, "buy", "0.001", "1000", idempotency_key="e2e-same-key-1")
        two = api.orders.limit(instrument, "buy", "0.001", "1000", idempotency_key="e2e-same-key-1")
        assert one.id == two.id
        api.orders.cancel(one.id)


def test_orders_carry_the_clients_book_and_reference(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        ack = api.orders.limit(instrument, "buy", "0.001", "1001", external_ref="ticket-7")
        order = api.orders.get(ack.id)
        assert order.book == "client:acme"
        assert order.external_ref == "ticket-7"
        api.orders.cancel(ack.id)
        algo = api.algos.sor("BTC/USDT", "buy", qty="0.001", external_ref="ticket-8")
        assert algo.state.book == "client:acme"
        assert algo.state.external_ref == "ticket-8"


def test_a_client_cannot_name_a_book(deployed):
    body = {"instrument_id": "1001", "side": "buy", "order_type": "market", "qty": "0.001", "book": "client:globex"}
    with client_of(deployed) as api, pytest.raises(OutOfScope):
        api.call("POST", "/orders", body=body)


def test_a_duplicate_is_denied_with_its_reason(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        first = api.orders.limit(instrument, "buy", "0.001", "1002")
        second = api.orders.limit(instrument, "buy", "0.001", "1002")
        denied = api.orders.get(second.id)
        assert denied.status.value == "denied"
        assert denied.refusal.code == "duplicate-order"
        assert denied.refusal.status == 409
        assert api.orders.get(first.id).is_open
        api.orders.cancel(first.id)


def test_a_client_sees_only_its_own(deployed):
    with client_of(deployed) as mine, client_of(deployed, "other") as theirs:
        instrument = btc(mine)
        order = mine.orders.limit(instrument, "buy", "0.001", "1000")
        with pytest.raises(NotFound):
            theirs.orders.get(order.id)
        with pytest.raises(NotFound):
            theirs.orders.cancel(order.id)
        assert all(row.id != order.id for row in theirs.orders.iter(open=True))
        assert any(row.id == order.id for row in mine.orders.iter(open=True))
        mine.orders.cancel(order.id)


def test_permissions_and_scopes_hold(deployed):
    with client_of(deployed, "reader") as reader:
        instrument = btc(reader)
        with pytest.raises(PermissionDenied):
            reader.orders.market(instrument, "buy", "0.001")
        with pytest.raises(PermissionDenied):
            reader.algos.sor("BTC/USDT", "buy", qty="0.001")
    with client_of(deployed, "narrow") as narrow:
        assert all(
            row.account == "binance" and row.local_symbol.startswith("BTC/")
            for row in narrow.market.instruments().items
        )
        with pytest.raises(OutOfScope):
            narrow.algos.sor("BTC/USDT", "buy", qty="0.001")
        with pytest.raises(OutOfScope):
            narrow.orders.market("1002", "buy", "0.001")
        with pytest.raises(OutOfScope):
            narrow.account.balances(account="okx")
        with pytest.raises(OrderRejected):
            narrow.orders.market("2001", "buy", "0.001", account="binance")


def test_bad_signatures_and_replays_are_refused_on_the_wire(deployed):
    with client_of(deployed) as api:
        assert api.whoami().client_id == "acme"
    with Client(deployed.url, key="full", secret="wrong-secret") as wrong, pytest.raises(AuthenticationError):
        wrong.whoami()
    stamp = str(int(time.time() * 1000))
    message = "\n".join((stamp, "GET", "/api/v1/whoami", "", ""))
    signature = hmac.new(deployed.secret.encode(), message.encode(), hashlib.sha256).hexdigest()
    headers = {"X-Raikiri-Key": "full", "X-Raikiri-Timestamp": stamp, "X-Raikiri-Signature": signature}
    assert httpx.get(deployed.url + "/api/v1/whoami", headers=headers).status_code == 200
    replayed = httpx.get(deployed.url + "/api/v1/whoami", headers=headers)
    assert replayed.status_code == 401
    assert replayed.json()["code"] == "replayed"
    assert replayed.headers["content-type"] == "application/problem+json"
    unsigned = httpx.post(deployed.url + "/api/v1/orders", content="{}")
    assert unsigned.status_code == 401


def test_rate_limits_answer_429_with_the_standard_headers(deployed):
    with client_of(deployed, "slow", retries=0) as slow:
        with pytest.raises(RateLimited) as caught:
            for _ in range(10):
                slow.whoami()
        assert caught.value.retry_after is not None and caught.value.retry_after >= 1


def test_amend_replaces_the_order_after_a_confirmed_cancel(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        order = api.orders.limit(instrument, "buy", "0.001", "1000")
        amended = api.orders.amend(order.id, price="1001")
        assert amended.replaces == order.id
        assert api.orders.get(order.id).status == "canceled"
        assert api.orders.get(amended.id).price == Decimal(1001)
        api.orders.cancel(amended.id)


def test_quotes_and_book_stream_through_apid(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        with api.stream(quotes=instrument, book=instrument, levels=3) as stream:
            seen: dict[type, object] = {}
            deadline = time.monotonic() + 10
            while len(seen) < 2 and time.monotonic() < deadline:
                event = stream.next(timeout=5)
                seen[type(event)] = event
        assert isinstance(seen[Quote], Quote) and seen[Quote].ask > seen[Quote].bid
        assert len(seen[Book].bids) == 3
        assert api.market.quote(instrument).instrument_id == instrument.instrument_id


def test_my_fills_stream_with_sequence_and_resume(deployed):
    with client_of(deployed) as api:
        instrument = btc(api)
        stream = api.stream(orders=True, fills=True, positions=True)
        while stream.next(timeout=0.5) is not None:  # connected, subscribed, and past what earlier tests left
            pass
        order = api.orders.market(instrument, "buy", "0.001")
        kinds = set()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not {OrderEvent, Fill, Position} <= kinds:
            event = stream.next(timeout=5)
            if getattr(event, "client_order_id", order.id) == order.id:
                kinds.add(type(event))
        assert {OrderEvent, Fill, Position} <= kinds
        last = stream.last_seq
        assert last is not None

        # drop the socket under it; a fill while it is gone comes back on resume
        stream._socket.close()
        missed = api.orders.market(instrument, "sell", "0.001")
        time.sleep(0.3)
        seen = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and seen is None:
            event = stream.next(timeout=5)
            if isinstance(event, Fill) and event.client_order_id == missed.id:
                seen = event
        stream.close()
        assert seen is not None
        assert seen.seq is not None and seen.seq > last
        assert stream.reconnects == 1


def test_another_clients_fill_never_reaches_my_stream(deployed):
    with client_of(deployed) as mine, client_of(deployed, "other") as theirs:
        instrument = btc(mine)
        stream = theirs.stream(fills=True, orders=True)
        while stream.next(timeout=0.5) is not None:
            pass
        mine.orders.market(instrument, "buy", "0.001")
        assert stream.next(timeout=1.0) is None
        stream.close()


def test_twap_with_progress_pause_and_child_attribution(deployed):
    with client_of(deployed) as api:
        algo = api.algos.twap("BTC/USDT", "buy", qty="0.004", horizon_s=1, slices=4)
        assert algo.state.client_id == "acme"
        states = list(algo.watch(interval=0.1))
        assert states[-1].status == "done"
        assert states[-1].filled == Decimal("0.004")
        assert states[-1].avg_price is not None
        assert states[-1].children
        child = states[-1].children[0].client_order_id
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            fills = [row for row in api.account.iter_fills() if row.parent_id == algo.id]
            if fills:
                break
            time.sleep(0.1)
        assert fills and fills[0].client_id == "acme"
        assert api.orders.get(child).parent_id == algo.id


def test_algo_pause_resume_cancel_and_scope(deployed):
    with client_of(deployed) as api, client_of(deployed, "other") as theirs:
        algo = api.algos.vwap("ETH/USDT", "sell", qty="0.05", horizon_s=30, slices=30, profile="u")
        assert algo.pause().status == "paused"
        with pytest.raises(NotFound):
            theirs.algos.cancel(algo.id)
        assert algo.resume().status == "working"
        assert algo.cancel().status == "cancelled"
        assert all(row.id != algo.id for row in theirs.algos.list())
        assert any(row.id == algo.id for row in api.algos.list())


def test_algo_updates_arrive_on_the_stream(deployed):
    with client_of(deployed) as api:
        stream = api.stream(algos=True)
        while stream.next(timeout=0.3) is not None:
            pass
        algo = api.algos.sor("BTC/USDT", "buy", notional="100")
        deadline = time.monotonic() + 10
        seen = None
        while time.monotonic() < deadline:
            event = stream.next(timeout=5)
            if isinstance(event, AlgoOrder) and event.id == algo.id and event.done:
                seen = event
                break
        stream.close()
        assert seen is not None and seen.filled > 0


def test_option_chain_with_greeks(deployed):
    with client_of(deployed) as api:
        underlyings = api.options.underlyings()
        expiry = underlyings[0].expiries[0].date
        chain = api.options.chain("BTC", expiry)
        assert chain.rows[0].call.venues[0].delta is not None
        assert isinstance(chain.rows[0].strike, Decimal)
        assert chain.atm_strike is not None


async def test_the_async_client_on_the_same_path(deployed):
    async with AsyncClient(deployed.url, key="full", secret=deployed.secret) as api:
        instrument = await api.market.find("BTC/USDT", account="binance")
        order = await api.orders.limit(instrument, "buy", "0.001", "1000")
        assert (await api.orders.get(order.id)).status == "accepted"
        await api.orders.cancel(order.id)
        quote = await api.market.quote(instrument)
        assert quote.bid > 0
        stream = await api.stream(fills=True)
        async with stream:
            assert await stream.next(timeout=0.5) is None
            placed = await api.orders.market(instrument, "sell", "0.001")
            seen = None
            while seen is None:
                event = await stream.next(timeout=5)
                assert event is not None
                if isinstance(event, Fill) and event.client_order_id == placed.id:
                    seen = event
        assert seen.client_id == "acme"
        algo = await api.algos.sor("BTC/USDT", "sell", qty="0.001")
        states = [state async for state in algo.watch(interval=0.05)]
        assert states[-1].done
