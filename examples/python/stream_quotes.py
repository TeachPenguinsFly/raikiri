"""Print ten seconds of quotes for BTC on one account."""

import time

from common import connect

from raikiri import Quote

with connect() as client:
    btc = client.market.find("BTC/USDT", account="binance")
    until = time.monotonic() + 10
    with client.stream(quotes=btc, hz=2) as stream:
        while time.monotonic() < until:
            event = stream.next(timeout=1)
            if isinstance(event, Quote):
                print(f"{event.bid} / {event.ask}  mid {event.mid}")
