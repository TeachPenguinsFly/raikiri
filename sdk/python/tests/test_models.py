from decimal import Decimal

import pytest

from raikiri import AlgoOrder, Fill, Order, OrderStatus, Quote, Side, sign, to_decimal
from raikiri._calls import place, start_algo
from raikiri.models import build


def test_signature_matches_the_canonical_form():
    # the same vector is in the c++ sdk's test, so the two agree byte for byte
    message = sign("secret", "1700000000000", "POST", "/api/v1/orders", "a=1", '{"x":1}')
    assert message == "da1a88d631b9aa2a64bfb7d33817ed791faece5478309426d52a6610fcf3744f"


def test_floats_are_refused_everywhere_money_goes_in():
    for bad in (0.1, True):
        with pytest.raises(TypeError):
            to_decimal(bad)
    with pytest.raises(TypeError):
        place("1", "buy", 0.1)
    with pytest.raises(TypeError):
        place("1", "buy", "1", price=100.5)
    with pytest.raises(TypeError):
        start_algo("twap", "BTC/USDT", "buy", qty=1.5)
    with pytest.raises(ValueError):
        to_decimal(Decimal("NaN"))


def test_decimals_go_out_exactly():
    body = place("1", Side.BUY, Decimal("1E-3"), price="68000.10").body
    assert body["qty"] == "0.001"
    assert body["price"] == "68000.10"
    assert body["order_type"] == "limit"
    assert place("1", "sell", 1).body["order_type"] == "market"
    with pytest.raises(ValueError):
        place("1", "hold", 1)


def test_algo_needs_exactly_one_size():
    with pytest.raises(ValueError):
        start_algo("twap", "BTC/USDT", "buy")
    with pytest.raises(ValueError):
        start_algo("twap", "BTC/USDT", "buy", qty="1", notional="100")


def test_rows_parse_to_exact_types_and_ignore_new_fields():
    order = build(
        Order,
        {
            "client_order_id": "7",
            "instrument_id": "1",
            "side": "buy",
            "order_type": "limit",
            "tif": "gtc",
            "qty": "1.000000001",
            "leaves": "0.5",
            "status": "accepted",
            "ts_created": "1700000000000000001",
            "ts_updated": "1700000000000000002",
            "a_field_from_the_future": {"x": 1},
        },
    )
    assert order.qty == Decimal("1.000000001")
    assert isinstance(order.filled, Decimal)
    assert order.filled == Decimal("0.500000001")
    assert order.ts_created == 1700000000000000001
    assert order.side is Side.BUY
    assert order.status is OrderStatus.ACCEPTED
    assert order.is_open


def test_a_status_newer_than_the_package_stays_readable():
    order = build(
        Order,
        {
            "client_order_id": "7",
            "instrument_id": "1",
            "side": "buy",
            "order_type": "limit",
            "tif": "gtc",
            "qty": "1",
            "status": "quantum",
            "ts_created": "1",
            "ts_updated": "1",
        },
    )
    assert order.status == "quantum"


def test_quote_mid_and_algo_progress_are_exact():
    quote = build(
        Quote, {"instrument_id": "1", "bid": "1.1", "bid_size": "1", "ask": "1.2", "ask_size": "1", "ts_event": "1"}
    )
    assert quote.mid == Decimal("1.15")
    algo = build(
        AlgoOrder,
        {
            "id": "1",
            "symbol": "s",
            "market": "perp",
            "side": "buy",
            "algo": "twap",
            "profile": "flat",
            "status": "working",
            "qty": "4",
            "filled": "1",
            "working": "0",
            "fees": "0",
            "horizon_s": 1,
            "slices": 1,
            "slices_fired": 1,
            "pov_bp": 0,
            "created": "1",
            "child_count": 0,
        },
    )
    assert algo.progress == Decimal("0.25")
    assert not algo.done


def test_a_crossed_fill_says_it_is_internal() -> None:
    fill = build(
        Fill,
        {
            "venue": "binance",
            "exec_id": "ixab",
            "instrument_id": "1",
            "side": "buy",
            "price": "100",
            "qty": "1",
            "ts_event": "6",
            "liquidity": "internal",
            "internal": True,
        },
    )
    assert fill.internal is True
    venue_fill = build(
        Fill,
        {
            "venue": "binance",
            "exec_id": "v",
            "instrument_id": "1",
            "side": "buy",
            "price": "100",
            "qty": "1",
            "ts_event": "6",
        },
    )
    assert venue_fill.internal is False
