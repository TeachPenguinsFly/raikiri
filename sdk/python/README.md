# raikiri (Python)

The client for raikiri's trading API. It needs Python 3.10 or newer, `httpx` and `websockets`.

```sh
uv add ./sdk/python     # or: pip install ./sdk/python
```

```python
from raikiri import Client

client = Client()  # RAIKIRI_API_URL, RAIKIRI_API_KEY, RAIKIRI_API_SECRET
btc = client.market.find("BTC/USDT", account="binance")
order = client.orders.limit(btc, "buy", "0.001", "60000")
print(client.orders.get(order.id).status)
```

With no URL it talks to `http://127.0.0.1:8900`, where `examples/paper/paper.py` runs a paper deployment. `AsyncClient`
is the same with `await`.

## Numbers

Prices and quantities are `Decimal` going out and coming back. A `float` raises `TypeError`, because `0.1` is not one
tenth. `str`, `int` and `Decimal` all work as input. Timestamps are unix nanoseconds as `int`, and ids are `str`.

## Orders

```python
client.orders.limit(btc, "buy", "0.001", "60000", post_only=True)
client.orders.market(btc, "sell", "0.001", reduce_only=True)
client.orders.cancel(order.id)
client.orders.amend(order.id, price="60100")
client.orders.cancel_all(instrument=btc)
client.orders.get(order.id); client.orders.events(order.id)
client.orders.wait(order.id)                    # poll until it is closed
for row in client.orders.iter(open=True): ...   # every page
```

An ack means the desk queued it, not that a venue took it. Risk refusals and venue rejections arrive as order status.

`amend` cancels, waits up to two seconds for the cancel to be confirmed, then places what remains under a new id.
Nothing rests in between and it loses queue position. If the cancel is not confirmed it raises `Conflict`
(`amend-unconfirmed`) and places nothing, so look at the order before trying again.

## Algos

```python
algo = client.algos.twap("BTC/USDT", "buy", qty="2", horizon_s=600, slices=20)
algo = client.algos.vwap("BTC/USDT", "buy", notional="50000", horizon_s=600, slices=20, profile="u")
algo = client.algos.sor("BTC/USDT", "buy", qty="1", limit="68000")
algo = client.algos.pov("BTC/USDT", "buy", qty="2", pov_bp=1000)  # 10 percent of volume

for state in algo.watch():
    print(state.status, state.progress, state.avg_price, state.slippage_arrival_bp)
algo.pause(); algo.resume(); algo.cancel()
```

An algo needs the `algo` permission and a key with every account, because the router picks venues across all of them.
If strategyd restarts it forgets running algos: calls raise `AlgoLost` and the stream sends `AlgoLostEvent`. Their
children remain ordinary orders and fills.

## Account and market data

```python
client.account.balances(); client.account.positions(); client.account.iter_fills()
client.market.instruments(account="binance"); client.market.markets("BTC"); client.market.quote(btc)
client.options.underlyings(); client.options.chain("BTC", "20261127")
client.whoami()
```

## Streams

```python
for event in client.stream(quotes=btc, book=btc, fills=True, positions=True):
    match event:
        case Quote(): ...
        case Fill(): ...
        case Gap(): ...  # refetch over REST
```

`async for event in await client.stream(...)` on `AsyncClient`. `stream.next(timeout=1.0)` returns `None` on a quiet
second, and `stream.subscribe(...)` adds more later.

The socket reconnects by itself with backoff, sends its subscriptions again, and resumes the sequenced channels (orders,
fills, positions, balances, algos) from the last number it saw. It reconnects after three missed heartbeats too. If the
server no longer holds what you missed, or restarted, you get a `Gap` and should refetch. Market channels are the current
value, so a slow reader gets fewer of them, not older ones. A 401 or 403 on connecting raises rather than looping.

## Errors and retries

Everything raises a subclass of `RaikiriError`: `AuthenticationError`, `PermissionDenied`, `OutOfScope`, `NotFound`,
`Conflict`, `OrderRejected`, `RateLimited`, `OutcomeUnknown`, `AlgoLost`, `ServerError`, `TransportError`. An `APIError`
has `status`, a stable `code`, `title`, `detail`, `request_id` and `retry_after`.

Reads retry on network errors, 429, 502 and 503, up to `retries` times, honouring `Retry-After`. So do mutations: each
call makes one idempotency key and keeps it through its retries, so the server applies it once. Pass your own
`idempotency_key` to carry that across a restart of your program.

`OutcomeUnknown` is the one to read twice. It means the server handed the call on and got no answer, so it may have
happened. It is never retried. Look at your orders, and send it again under a new key only if it did not.

A clock outside the server's window is measured and corrected once per call by itself.

## Logging

The package logs at debug to `raikiri` and `raikiri.stream`. For metrics, pass hooks; they never see the secret or the
signature:

```python
Client(hooks=Hooks(on_response=lambda r: print(r.method, r.url, r.status, r.elapsed_s, r.rate_limit_remaining)))
```

## Tests

`uv run pytest`. The suite runs against a fake server. With `apid` built, it also runs every test in `test_e2e.py` and
every example in both languages through a real `apid` and the paper backend.
