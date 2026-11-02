from __future__ import annotations

import asyncio
import json
import time
from decimal import Decimal

import pytest
from fake_server import FakeServer

from raikiri import (
    AlgoLost,
    AsyncClient,
    AuthenticationError,
    Client,
    Hooks,
    IdempotencyMismatch,
    NotFound,
    OrderRejected,
    OutcomeUnknown,
    OutOfScope,
    PermissionDenied,
    RaikiriError,
    RateLimited,
    ServerError,
    TransportError,
)

ACK = {"client_order_id": "42", "status": "queued", "client_id": "acme"}
ROW = {
    "client_order_id": "42",
    "instrument_id": "1",
    "side": "buy",
    "order_type": "limit",
    "tif": "gtc",
    "qty": "1",
    "leaves": "1",
    "status": "accepted",
    "ts_created": "1",
    "ts_updated": "1",
}
INSTRUMENT = {
    "instrument_id": "1",
    "account": "binance",
    "venue": "binance",
    "market": "perp",
    "exchange_symbol": "BTCUSDT",
    "local_symbol": "BTC/USDT",
    "tick_size": "0.1",
    "lot_size": "0.001",
    "min_notional": "5",
    "contract_value": "1",
    "contract_kind": "linear",
    "asset_class": "crypto",
}


def problem(status: int, code: str, **extra):
    return status, {"status": status, "code": code, "title": code, **extra}


def client(server: FakeServer, credentials, **options) -> Client:
    return Client(
        server.url, retries=options.pop("retries", 3), timeout=options.pop("timeout", 5), **credentials, **options
    )


def test_place_signs_and_sends_an_idempotency_key(server, credentials):
    server.routes["POST /orders"] = lambda seen: (202, ACK)
    with client(server, credentials) as api:
        ack = api.orders.limit("1", "buy", "0.5", "68000")
    assert ack.id == "42"
    seen = server.seen[0]
    assert json.loads(seen.body)["price"] == "68000"
    assert len(seen.headers["Idempotency-Key"]) >= 8


def test_an_instrument_object_supplies_its_account(server, credentials):
    server.routes["POST /orders"] = lambda seen: (202, ACK)
    server.routes["GET /instruments"] = lambda seen: (200, {"items": [INSTRUMENT]})
    with client(server, credentials) as api:
        btc = api.market.find("BTC/USDT")
        api.orders.market(btc, "sell", 1)
    assert btc.tick_size == Decimal("0.1")
    assert json.loads(server.seen[-1].body)["account"] == "binance"


def test_retry_of_a_mutation_keeps_its_key_and_signs_afresh(server, credentials):
    server.routes["POST /orders"] = [
        lambda seen: problem(503, "backend-unavailable"),
        lambda seen: (429, {"status": 429, "code": "rate-limited", "title": "slow"}, {"Retry-After": "0"}),
        lambda seen: (202, ACK),
    ]
    with client(server, credentials) as api:
        assert api.orders.market("1", "buy", 1).id == "42"
    keys = {seen.headers["Idempotency-Key"] for seen in server.seen}
    signatures = {seen.headers["X-Raikiri-Signature"] for seen in server.seen}
    assert len(server.seen) == 3
    assert len(keys) == 1
    assert len(signatures) == 3


def test_retry_after_is_honoured(server, credentials):
    server.routes["GET /orders/42"] = [
        lambda seen: (429, {"status": 429, "code": "rate-limited", "title": "slow"}, {"Retry-After": "1"}),
        lambda seen: (200, ROW),
    ]
    started = time.monotonic()
    with client(server, credentials) as api:
        assert api.orders.get("42").id == "42"
    assert time.monotonic() - started >= 1.0


def test_an_unknown_outcome_is_never_retried(server, credentials):
    server.routes["POST /orders"] = lambda seen: problem(504, "backend-timeout")
    with client(server, credentials) as api, pytest.raises(OutcomeUnknown) as caught:
        api.orders.market("1", "buy", 1)
    assert len(server.seen) == 1
    assert caught.value.status == 504


@pytest.mark.parametrize(
    ("status", "code", "kind"),
    [
        (401, "auth-failed", AuthenticationError),
        (403, "permission-denied", PermissionDenied),
        (403, "out-of-scope", OutOfScope),
        (403, "ip-not-allowed", OutOfScope),
        (404, "not-found", NotFound),
        (409, "outcome-unknown", OutcomeUnknown),
        (410, "algo-lost", AlgoLost),
        (422, "order-refused", OrderRejected),
        (422, "account-mismatch", OrderRejected),
        (422, "idempotency-mismatch", IdempotencyMismatch),
        (500, "whatever", ServerError),
    ],
)
def test_problems_become_typed_exceptions(server, credentials, status, code, kind):
    server.routes["POST /orders"] = lambda seen: problem(status, code, detail="because", request_id="r1")
    with client(server, credentials) as api, pytest.raises(kind) as caught:
        api.orders.market("1", "buy", 1)
    assert caught.value.code == code
    assert caught.value.detail == "because"
    assert caught.value.request_id == "r1"
    assert len(server.seen) == 1


def test_a_replay_refusal_is_signed_again(server, credentials):
    server.routes["GET /orders/42"] = [lambda seen: problem(401, "replayed"), lambda seen: (200, ROW)]
    with client(server, credentials) as api:
        assert api.orders.get("42").id == "42"
    assert server.seen[0].headers["X-Raikiri-Timestamp"] != server.seen[1].headers["X-Raikiri-Timestamp"]


def test_rate_limited_after_retries_carries_retry_after(server, credentials):
    server.routes["GET /orders/42"] = lambda seen: (
        429,
        {"status": 429, "code": "rate-limited", "title": "x"},
        {"Retry-After": "0"},
    )
    with client(server, credentials, retries=1) as api, pytest.raises(RateLimited) as caught:
        api.orders.get("42")
    assert caught.value.retry_after == 0
    assert len(server.seen) == 2


def test_an_in_flight_key_is_retried(server, credentials):
    server.routes["POST /orders"] = [
        lambda seen: (409, {"status": 409, "code": "idempotency-in-flight", "title": "x"}, {"Retry-After": "0"}),
        lambda seen: (202, ACK),
    ]
    with client(server, credentials) as api:
        assert api.orders.market("1", "buy", 1).id == "42"


def test_a_skewed_clock_is_measured_once_and_retried(server, credentials):
    calls = []

    def skew_once(seen):
        calls.append(seen)
        if len(calls) == 1:
            return problem(401, "timestamp-skew")
        return 200, ROW

    server.clock_offset_ms = 60_000
    server.routes["GET /orders/42"] = skew_once
    with client(server, credentials) as api:
        assert api.orders.get("42").id == "42"
    first, second = calls
    assert int(second.headers["X-Raikiri-Timestamp"]) - int(first.headers["X-Raikiri-Timestamp"]) > 50_000


def test_a_dead_server_is_a_transport_error(credentials):
    with Client("http://127.0.0.1:9", retries=1, timeout=1, **credentials) as api, pytest.raises(TransportError):
        api.orders.get("1")


def test_pagination_walks_every_page(server, credentials):
    server.routes["GET /orders"] = [
        lambda seen: (200, {"items": [ROW], "next_cursor": "c2"}),
        lambda seen: (200, {"items": [], "next_cursor": "c3"}),
        lambda seen: (200, {"items": [ROW]}),
    ]
    with client(server, credentials) as api:
        assert len(list(api.orders.iter(open=True))) == 2
    assert "open=true" in server.seen[0].query
    assert "cursor=c2" in server.seen[1].query
    assert "cursor=c3" in server.seen[2].query


def test_query_values_are_escaped_and_still_verify(server, credentials):
    server.routes["GET /markets"] = lambda seen: (200, {"items": []})
    with client(server, credentials) as api:
        api.market.markets("BTC/USDT & co")
    assert "%2F" in server.seen[0].query and "%20" in server.seen[0].query


def test_find_refuses_ambiguity(server, credentials):
    other = {**INSTRUMENT, "instrument_id": "2", "account": "okx"}
    server.routes["GET /instruments"] = lambda seen: (200, {"items": [INSTRUMENT, other]})
    with client(server, credentials) as api, pytest.raises(RaikiriError, match="binance, okx"):
        api.market.find("BTC/USDT")


def test_hooks_see_the_call_and_never_the_secret(server, credentials):
    requests, responses = [], []
    server.routes["POST /orders"] = lambda seen: (202, ACK, {"RateLimit-Remaining": "7", "Idempotent-Replay": "true"})
    with client(server, credentials, hooks=Hooks(requests.append, responses.append)) as api:
        api.orders.market("1", "buy", 1)
    assert requests[0].method == "POST"
    assert responses[0].status == 202
    assert responses[0].replayed
    assert responses[0].rate_limit_remaining == 7
    assert credentials["secret"] not in repr(requests) + repr(responses) + repr(api._runner.settings)


def test_algo_handle_controls_and_watches(server, credentials):
    base = {
        "id": "9",
        "symbol": "BTC/USDT",
        "market": "perp",
        "side": "buy",
        "algo": "twap",
        "profile": "flat",
        "status": "working",
        "qty": "2",
        "filled": "0",
        "working": "0",
        "fees": "0",
        "horizon_s": 60,
        "slices": 6,
        "slices_fired": 0,
        "pov_bp": 0,
        "created": "1",
        "child_count": 0,
    }
    server.routes["POST /algos"] = lambda seen: (201, base)
    server.routes["POST /algos/9/pause"] = lambda seen: (200, {**base, "status": "paused"})
    server.routes["POST /algos/9/resume"] = lambda seen: (200, base)
    server.routes["GET /algos/9"] = [
        lambda seen: (200, {**base, "filled": "1", "slices_fired": 3}),
        lambda seen: (200, {**base, "filled": "1", "slices_fired": 3}),
        lambda seen: (200, {**base, "filled": "2", "slices_fired": 6, "status": "done"}),
    ]
    with client(server, credentials) as api:
        algo = api.algos.twap("BTC/USDT", "buy", qty="2", horizon_s=60, slices=6)
        assert algo.pause().status == "paused"
        assert algo.resume().status == "working"
        seen = list(algo.watch(interval=0))
    assert [state.filled for state in seen] == [Decimal(1), Decimal(2)]
    assert seen[-1].done
    assert json.loads(server.seen[0].body) == {
        "symbol": "BTC/USDT",
        "market": "perp",
        "side": "buy",
        "algo": "twap",
        "qty": "2",
        "horizon_s": 60,
        "slices": 6,
    }


def test_from_the_environment(monkeypatch, server, credentials):
    monkeypatch.setenv("RAIKIRI_API_URL", server.url)
    monkeypatch.setenv("RAIKIRI_API_KEY", credentials["key"])
    monkeypatch.setenv("RAIKIRI_API_SECRET", credentials["secret"])
    server.routes["GET /orders/42"] = lambda seen: (200, ROW)
    with Client() as api:
        assert api.orders.get("42").status == "accepted"
    monkeypatch.delenv("RAIKIRI_API_SECRET")
    with pytest.raises(RaikiriError, match="RAIKIRI_API_SECRET"):
        Client()


async def test_async_place_retry_and_errors(server, credentials):
    server.routes["POST /orders"] = [lambda seen: problem(503, "backend-unavailable"), lambda seen: (202, ACK)]
    server.routes["GET /orders/42"] = lambda seen: problem(404, "not-found")
    async with AsyncClient(server.url, **credentials) as api:
        ack = await api.orders.limit("1", "buy", "0.5", "68000")
        assert ack.id == "42"
        with pytest.raises(NotFound):
            await api.orders.get("42")
    assert server.seen[0].headers["Idempotency-Key"] == server.seen[1].headers["Idempotency-Key"]


async def test_async_algo_watch(server, credentials):
    base = {
        "id": "9",
        "symbol": "BTC/USDT",
        "market": "perp",
        "side": "buy",
        "algo": "sor",
        "profile": "flat",
        "status": "working",
        "qty": "2",
        "filled": "0",
        "working": "0",
        "fees": "0",
        "horizon_s": 0,
        "slices": 1,
        "slices_fired": 0,
        "pov_bp": 0,
        "created": "1",
        "child_count": 0,
    }
    server.routes["POST /algos"] = lambda seen: (201, base)
    server.routes["GET /algos/9"] = lambda seen: (200, {**base, "status": "done", "filled": "2"})
    async with AsyncClient(server.url, **credentials) as api:
        algo = await api.algos.sor("BTC/USDT", "buy", qty="2")
        states = [state async for state in algo.watch(interval=0)]
    assert states[-1].progress == Decimal(1)
    await asyncio.sleep(0)
