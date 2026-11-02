"""Place a limit order well below the market, look at it, and cancel it."""

from common import connect

with connect() as client:
    btc = client.market.find("BTC/USDT", account="binance")
    quote = client.market.quote(btc)
    price = (quote.bid * 9 / 10 // btc.tick_size) * btc.tick_size

    ack = client.orders.limit(btc, "buy", btc.lot_size, price)
    print(f"placed {ack.id} at {price}, market bid is {quote.bid}")

    order = client.orders.get(ack.id)
    print(f"status {order.status.value}, resting {order.leaves}")

    client.orders.cancel(ack.id)
    print(f"final status {client.orders.wait(ack.id).status.value}")
