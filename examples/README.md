# Examples

The same eight programs in [Python](python/README.md) and [C++](cpp/README.md):

| | |
|---|---|
| `place_and_cancel` | Place a limit order, look at it, cancel it |
| `stream_quotes` | Print quotes as they arrive |
| `stream_fills` | Watch your own fills and position while an order trades |
| `twap` | TWAP a large order, watch its progress, pause and resume it |
| `sor_slippage` | Smart-route an order and read its slippage against the mid on arrival |
| `option_chain` | An expiry's chain with each strike's greeks |
| `market_maker` | Requote on each book update, with a position limit |
| `stale_price_guard` | Pull your orders when quotes stop arriving |

## The paper deployment

`paper/paper.py` starts a real `apid` in front of `paper/paper_backend.py`, an in-memory stand-in for admind with a
random-walk market that fills against itself. No venue is involved, and the prices are made up. It needs `apid` built:

```sh
cmake --build build/dev-asan --target apid     # or set RAIKIRI_APID to a binary
cd sdk/python && uv sync && cd ../..
uv run --project sdk/python python examples/paper/paper.py
```

It listens on `http://127.0.0.1:8900`, draws a new secret, and prints the three `export` lines to paste into another
shell. Nothing is kept between runs. Its stand-in for joe slices over real time and is not joe: use it to learn the API,
not to judge execution.

## Against a real deployment

Set `RAIKIRI_API_URL`, `RAIKIRI_API_KEY` and `RAIKIRI_API_SECRET`. These programs place orders, and they refuse any URL
that is not loopback unless you also set `RAIKIRI_EXAMPLES_LIVE=yes`. None of them holds a key or a secret.
