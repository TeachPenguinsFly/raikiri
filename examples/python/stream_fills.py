"""Watch this client's fills and position while a small order trades.

The stream resumes by itself: drop the network under it and the fills you missed still arrive, in order.
"""

import time

from common import connect

from raikiri import Fill, Gap, Position

with connect() as client:
    btc = client.market.find("BTC/USDT", account="binance")
    with client.stream(fills=True, positions=True) as stream:
        stream.next(timeout=0.5)  # let the subscription land before trading
        client.orders.market(btc, "buy", btc.lot_size)
        client.orders.market(btc, "sell", btc.lot_size)

        until = time.monotonic() + 5
        while time.monotonic() < until:
            event = stream.next(timeout=1)
            if isinstance(event, Fill):
                print(f"#{event.seq} fill {event.side.value} {event.qty} at {event.price}, fee {event.fee}")
            elif isinstance(event, Position):
                print(f"#{event.seq} position {event.symbol} {event.qty}, realized {event.realized}")
            elif isinstance(event, Gap):
                print(f"missed events ({event.reason}); refetch: {client.account.positions()}")
