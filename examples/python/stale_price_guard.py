"""Pull your orders when the price stops moving through: a stalled feed is when a resting quote gets picked off.

Age is measured here, from when each quote arrived, so a skewed clock on either side cannot hide a stall.
"""

import time
from decimal import Decimal

from common import connect

from raikiri import Quote

MAX_QUOTE_AGE_S = Decimal("2")
RUN_SECONDS = 8

with connect() as client:
    btc = client.market.find("BTC/USDT", account="binance")
    last_quote_at = time.monotonic()
    guarded = False
    with client.stream(quotes=btc, hz=5) as stream:
        quote = stream.next(timeout=5)
        assert isinstance(quote, Quote)
        far = ((quote.bid * 9 / 10) // btc.tick_size) * btc.tick_size
        order = client.orders.limit(btc, "buy", btc.lot_size, far)
        print(f"resting {order.id} at {far}")

        until = time.monotonic() + RUN_SECONDS
        while time.monotonic() < until:
            event = stream.next(timeout=0.25)
            if isinstance(event, Quote):
                last_quote_at = time.monotonic()
                if guarded:
                    print("quotes are back; a real strategy would requote here")
                    guarded = False
            age = Decimal(str(time.monotonic() - last_quote_at))
            if age > MAX_QUOTE_AGE_S and not guarded:
                guarded = True
                done = client.orders.cancel_all(instrument=btc)
                print(f"no quote for {age:.1f}s; canceled {done.canceled}")

    client.orders.cancel_all(instrument=btc)
    print("no stall in this run" if not guarded else "guard fired")
