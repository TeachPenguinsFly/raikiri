"""A small quoting loop: requote around the mid on each book update, and stop adding past a position limit.

It shows the mechanics, not a strategy. On the paper backend it trades against a random walk.
"""

import time
from decimal import Decimal

from common import connect

from raikiri import Book, Conflict, NotFound, Position

HALF_SPREAD_TICKS = 3
REQUOTE_TICKS = 2
POSITION_LIMIT = Decimal("0.005")
RUN_SECONDS = 15

with connect() as client:
    btc = client.market.find("BTC/USDT", account="binance")
    tick, size = btc.tick_size, btc.lot_size
    position = next((p.qty for p in client.account.positions(instrument=btc)), Decimal(0))
    quoted_mid = None
    resting: dict[str, str] = {}

    try:
        with client.stream(book=btc, levels=1, hz=5, positions=True) as stream:
            until = time.monotonic() + RUN_SECONDS
            while time.monotonic() < until:
                event = stream.next(timeout=1)
                if isinstance(event, Position) and event.instrument_id == btc.instrument_id:
                    position = event.qty
                    print(f"position {position}")
                if not isinstance(event, Book) or not event.bids or not event.asks:
                    continue
                mid = (event.bids[0].price + event.asks[0].price) / 2
                if quoted_mid is not None and abs(mid - quoted_mid) < REQUOTE_TICKS * tick:
                    continue
                quoted_mid = mid

                for order_id in resting.values():
                    try:
                        client.orders.cancel(order_id)
                    except (Conflict, NotFound):
                        pass  # it filled first; the position update will say so
                resting.clear()

                bid = ((mid - HALF_SPREAD_TICKS * tick) // tick) * tick
                ask = ((mid + HALF_SPREAD_TICKS * tick) // tick) * tick
                if position < POSITION_LIMIT:
                    resting["buy"] = client.orders.limit(btc, "buy", size, bid, post_only=True).id
                if position > -POSITION_LIMIT:
                    resting["sell"] = client.orders.limit(btc, "sell", size, ask, post_only=True).id
                print(f"quoting {bid} / {ask} around {mid}, position {position}")
    finally:
        done = client.orders.cancel_all(instrument=btc)
        print(f"stopped; canceled {len(done.canceled)} orders")
