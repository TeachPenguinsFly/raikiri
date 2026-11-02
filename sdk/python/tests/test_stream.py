from __future__ import annotations

from decimal import Decimal

import pytest
from fake_server import FakeServer

from raikiri import AsyncClient, AuthenticationError, Client, Fill, Gap, OrderEvent, Position, Quote

QUOTE = {
    "type": "quotes",
    "id": "m1",
    "ts": "1",
    "quotes": [{"instrument_id": "1", "bid": "1.5", "bid_size": "2", "ask": "1.6", "ask_size": "3", "ts_event": "1"}],
}


def fill(seq: int, exec_id: str) -> dict:
    return {
        "type": "fill",
        "seq": str(seq),
        "fill": {
            "venue": "binance",
            "exec_id": exec_id,
            "instrument_id": "1",
            "side": "buy",
            "price": "100.5",
            "qty": "1",
            "ts_event": "5",
            "client_id": "acme",
        },
    }


def client(server: FakeServer, credentials) -> Client:
    # the stream listens on its own port here; apid serves both on one
    return Client(server.ws_url, **credentials)


def test_a_skewed_clock_on_the_upgrade_is_measured_and_retried(credentials, monkeypatch):
    server = FakeServer(skew_upgrades=1, clock_offset_ms=60_000, frames=[[fill(1, "a")]])
    server.start()
    real_get = __import__("httpx").get
    # /time is on the rest port in this fake; apid serves both on one
    monkeypatch.setattr(
        "raikiri.stream.httpx.get", lambda url, **kw: real_get(url.replace(server.ws_url, server.url), **kw)
    )
    try:
        with client(server, credentials) as api, api.stream(fills=True) as stream:
            assert stream.next(timeout=5).exec_id == "a"
            assert stream._signer.clock_offset_ms > 50_000
    finally:
        server.stop()


def test_a_second_skew_refusal_is_not_retried_forever(credentials):
    server = FakeServer(skew_upgrades=5)
    server.start()
    try:
        with client(server, credentials) as api, pytest.raises(AuthenticationError):
            api.stream(fills=True).next(timeout=5)
    finally:
        server.stop()


def test_market_events_are_typed(server, credentials):
    server.frames = [[QUOTE]]
    with client(server, credentials) as api, api.stream(quotes="1") as stream:
        event = stream.next(timeout=5)
    assert isinstance(event, Quote)
    assert event.mid == Decimal("1.55")


def test_private_subscribe_is_sent_with_the_channels(server, credentials):
    server.frames = [[fill(1, "a")]]
    with client(server, credentials) as api, api.stream(fills=True, positions=True) as stream:
        event = stream.next(timeout=5)
    assert isinstance(event, Fill)
    assert event.price == Decimal("100.5")
    assert event.seq == 1
    assert server.received[0][0]["channels"] == ["fills", "positions"]
    assert "resume_from" not in server.received[0][0]


def test_reconnect_resubscribes_and_resumes_from_the_last_sequence(server, credentials):
    server.frames = [[fill(3, "a"), "drop"], [fill(4, "b")]]
    with client(server, credentials) as api, api.stream(quotes="1", fills=True) as stream:
        first = stream.next(timeout=5)
        second = stream.next(timeout=5)
        assert stream.reconnects == 1
    assert first.exec_id == "a"
    assert second.exec_id == "b"
    resumed = server.received[1][0]
    assert resumed["resume_from"] == "3"
    assert resumed["epoch"] == "e1"
    assert resumed["channels"] == ["fills"]


def test_a_replayed_sequence_is_not_delivered_twice(server, credentials):
    server.frames = [[fill(5, "a"), fill(5, "a"), fill(6, "b")]]
    with client(server, credentials) as api, api.stream(fills=True) as stream:
        assert stream.next(timeout=5).exec_id == "a"
        assert stream.next(timeout=5).exec_id == "b"


def test_a_gap_is_an_event_and_voids_the_resume_point(server, credentials):
    server.frames = [
        [fill(9, "a"), "drop"],
        [{"type": "gap", "from": "10", "to": "20", "reason": "gone"}, fill(21, "b"), "drop"],
        [],
    ]
    with client(server, credentials) as api, api.stream(fills=True) as stream:
        assert stream.next(timeout=5).exec_id == "a"
        gap = stream.next(timeout=5)
        assert isinstance(gap, Gap)
        assert (gap.from_seq, gap.to_seq) == (10, 20)
        assert stream.next(timeout=5).exec_id == "b"
        assert stream.next(timeout=1) is None
    assert server.received[2][0]["resume_from"] == "21"


def test_silence_past_three_heartbeats_reconnects(server, credentials):
    server.heartbeat_ms = 100
    server.frames = [["silence"], [fill(1, "after")]]
    with client(server, credentials) as api, api.stream(fills=True) as stream:
        assert stream.next(timeout=5).exec_id == "after"
        assert stream.reconnects == 1


def test_a_refused_upgrade_raises_instead_of_looping(server, credentials):
    server.upgrade_status = 401
    with client(server, credentials) as api, pytest.raises(AuthenticationError):
        api.stream(fills=True).next(timeout=5)


def test_every_private_channel_parses(server, credentials):
    server.frames = [
        [
            {
                "type": "order",
                "seq": "1",
                "order_event": {
                    "client_order_id": "7",
                    "instrument_id": "1",
                    "event_type": "accepted",
                    "ts_event": "1",
                },
            },
            {
                "type": "position",
                "seq": "2",
                "position": {
                    "client_id": "c",
                    "account": "a",
                    "symbol": "s",
                    "instrument_id": "1",
                    "qty": "-1.5",
                    "realized": "0",
                    "fees": "0",
                    "ts_updated": "1",
                },
            },
            {"type": "heartbeat", "ts": "1", "seq": "2"},
            {"type": "something_new", "seq": "3"},
        ]
    ]
    with client(server, credentials) as api, api.stream(orders=True, positions=True) as stream:
        order = stream.next(timeout=5)
        position = stream.next(timeout=5)
    assert isinstance(order, OrderEvent)
    assert isinstance(position, Position)
    assert position.qty == Decimal("-1.5")


def test_subscribing_later_sends_only_what_is_new(server, credentials):
    server.frames = [[QUOTE]]
    with client(server, credentials) as api, api.stream(quotes="1") as stream:
        assert isinstance(stream.next(timeout=5), Quote)
        stream.subscribe(book="2", levels=3)
        stream.next(timeout=1)
    book = server.received[0][-1]
    assert book["book"] == "2"
    assert book["levels"] == 3
    assert "quotes" not in book


async def test_async_iteration_reconnects_and_resumes(server, credentials):
    server.frames = [[fill(1, "a"), "drop"], [fill(2, "b")]]
    async with AsyncClient(server.ws_url, **credentials) as api:
        stream = await api.stream(fills=True, quotes="1")
        seen = []
        async with stream:
            async for event in stream:
                seen.append(event.exec_id)
                if len(seen) == 2:
                    break
    assert seen == ["a", "b"]
    assert server.received[1][0]["resume_from"] == "1"


async def test_async_refused_upgrade_raises(server, credentials):
    server.upgrade_status = 403
    async with AsyncClient(server.ws_url, **credentials) as api:
        with pytest.raises(Exception, match="403"):
            await (await api.stream(fills=True)).next(timeout=5)
