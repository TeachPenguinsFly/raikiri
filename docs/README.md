# raikiri documentation

How to work with the framework. The [project README](../README.md) has the short version, and
[How raikiri works](../ARCHITECTURE.md) explains the processes, shared memory and the order path.

- [Write a strategy](#write-a-strategy): the callbacks and the risk gate
- [Client API](#client-api): trading from another machine, Python and C++ SDKs
- [Running a gateway](#running-a-gateway): flags and credentials
- [Configuration](#configuration): venue rate limits
- [Venue table](#venue-table): what each venue supports, adding a venue
- [Performance](#performance): measured numbers
- [Build and test](#build-and-test): presets and sanitizers
- [Releases](#releases): versions and compatibility
- [Status](#status): what works and what is still open

## Write a strategy

A strategy overrides the callbacks it needs and talks to the kernel through `Context`:

```cpp
#include <raikiri/strategy/factory.hpp>
#include <raikiri/strategy/strategy.hpp>

using namespace raikiri;

// rests one lot post-only at the best bid and follows the bid as it moves
class JoinBid final : public strategy::Strategy {
public:
    void on_bbo(strategy::Context& ctx, const Bbo& bbo) override {
        if (bbo.bid.is_undef() || bbo.bid == price_) {
            return;
        }
        if (resting_.valid()) {
            (void)ctx.cancel(resting_);
        }
        const auto id = ctx.submit({.instrument = bbo.instrument,
                                    .side = Side::Buy,
                                    .flags = OrderFlags::PostOnly,
                                    .price = bbo.bid,
                                    .qty = QtyLots::from_raw(1)});
        resting_ = id.value_or(ClientOrderId{});
        price_ = bbo.bid;
    }

private:
    ClientOrderId resting_{};
    PriceTicks price_{};
};

const bool registered = strategy::register_type<JoinBid>("join-bid");
```

```sh
strategyd --gateway=bybit --strategy join-bid
```

No new order passes until a person declares the desk's capital (`POST /v1/risk/capital`, or the Risk
page where one is deployed), so a fresh data directory refuses everything but cancels. Only sim seeds
itself. `--seed-capital <dollars>` sets a first figure from the command line and never replaces one a
person set.

Besides `on_bbo` there are `on_trade`, `on_funding`, `on_order_event`, `on_timer`, `on_start` and
`on_stop`. `Context` also gives the strategy its position, unrealised PnL, timers and `set_target`,
which declares the position to hold and lets the kernel send the orders.
[`demo_maker.hpp`](../libs/strategies/include/raikiri/strategies/demo_maker.hpp) is a complete two-sided
example. `make demo` runs the whole pipeline against the simulated venue and prints a latency table at the end.

## Client API

`apid` lets a program on another machine trade through a deployment. It sits in front of admind, which stays on
loopback, and adds what an open port needs: API keys with HMAC request signing, per-key permissions (`read`, `trade`,
`algo`), allowed accounts and symbols, rate limits, an IP allowlist, idempotency on every mutating call, and an audit
log. Orders still go through admind's desk and strategyd's risk gate. The contract is
[`schema/client-api.yaml`](../schema/client-api.yaml); admind's own is [`schema/admin-api.yaml`](../schema/admin-api.yaml).

```python
from raikiri import Client

client = Client()  # RAIKIRI_API_URL, RAIKIRI_API_KEY, RAIKIRI_API_SECRET
btc = client.market.find("BTC/USDT", account="binance")
order = client.orders.limit(btc, "buy", "0.001", "60000")
print(client.orders.get(order.id).status)
```

```cpp
#include <raikiri/sdk/client.hpp>
using namespace raikiri::sdk::literals;

int main() {
    auto client = *raikiri::sdk::Client::from_env();
    auto btc = *client.market().find("BTC/USDT", "binance");
    auto order = client.orders().limit(btc, raikiri::sdk::Side::Buy, "0.001"_d, "60000"_d);
}
```

Both SDKs have the same surface: orders, algos with live progress, account, market data, the option chain, and streams
that reconnect and resume from their last sequence number. Prices and quantities are exact decimals in both; neither
takes a float. See [`sdk/python`](../sdk/python/README.md) and [`sdk/cpp`](../sdk/cpp/README.md).

[`examples/`](../examples/README.md) has the same eight programs in each language, and a paper deployment to run them
against: a real `apid` in front of an in-memory backend, with no venue and no money.

```sh
cmake --build build/dev-asan --target apid
cd sdk/python && uv sync && cd ../..
uv run --project sdk/python python examples/paper/paper.py
```

Three things differ from what the names might suggest. `amend` is a confirmed cancel followed by a new order, because
the desk has no modify, so it loses queue position. A client's positions are folded from its own fills, not read from the
venue, since a shared account's venue position belongs to no one client. And balances are account-level: clients on one
account see the same numbers. Running it for real clients is in [`deploy/README.md`](../deploy/README.md#client-api).

## Running a gateway

Public market data needs no credentials.

```sh
make deps && make build

# one venue, one symbol, twenty seconds of live market data
build/dev-asan/apps/gatewayd/gatewayd --venue=bybit --symbol=BTCUSDT --duration 20
```

When a gateway stops it prints the instruments it loaded and its BBO, depth, trade and
funding counts. If a venue renames a channel or stops publishing a stream, the count for
it is zero; unit tests against fixtures will not notice.

Credentials are passed as references (`env:NAME`, `file:/path`, `exec:command` or
`op://vault/item/field`) and never as values, since anything on the command line is
visible to every user on the host through `ps`.

```sh
gatewayd --venue=bybit --market=perp \
         --api-key=env:BYBIT_KEY --api-secret=op://trading/bybit/secret
```

## Configuration

`config/venues.yaml` sets each venue's rate limits. `RAIKIRI_CONFIG_DIR` names the
directory that holds it (default: `config` under the working directory). A venue that
is not listed, or has no `limits`, is not limited. Each entry under `limits` has these keys:

| key | meaning |
| :-- | :-- |
| `name` | label used in logs and metrics |
| `kind` | what is counted, default `rest`; see below |
| `window_ms` | sliding window length for `rest`, `ws_messages` and `ws_connects` |
| `max` | most allowed in any stretch of `window_ms`, or the count ceiling for the other kinds |
| `reserve` | part of `max` only order traffic may spend (`rest` only) |
| `match` | path prefixes the limit meters (`rest` only); omitted means every request |
| `scope` | `socket` or `ip`, for `ws_messages` only |
| `market` | `perp` or `spot`; omitted applies to both |

| kind | counts |
| :-- | :-- |
| `rest` | REST cost per window over the paths in `match` |
| `ws_messages` | outbound socket frames per window, per `scope` |
| `ws_connects` | connection attempts per window, per ip |
| `ws_streams` | subscriptions one socket may carry |
| `ws_sockets` | sockets one ip may hold |
| `ws_channels` | subscriptions across all sockets of the ip |
| `ws_frame` | subscriptions one subscribe frame may carry |

A REST request spends from every `rest` limit whose `match` prefixes its path, or from
none of them if any one would refuse. The limits are sliding windows, which also stay
within a venue that counts fixed windows, and are set at about 90% of the venue's figure.
`costs` lists `{ match, cost }` rules in the venue's own units: the longest match
wins, a `?k=v` in a rule must appear in the query, and an unmatched request costs
`default_cost` (1 unless set).

## Venue table

Generated from [`libs/exec/src/caps.cpp`](../libs/exec/src/caps.cpp) by
[`scripts/venue_table.py`](../scripts/venue_table.py), which also draws the logo grid in the
[project README](../README.md#venues).

| column | meaning |
| :-- | :-- |
| `custody` | CEX if the venue holds your coins, DEX if a contract does. The caps table has no bit for this, so the generator keeps its own list and refuses a venue missing from it. |
| `order entry` | *market data only*: the adapter sends no orders. `grvt` signs orders with an EIP-712 EVM wallet this codebase has no signer for yet. `cryptocom` uses a plain HMAC key, but its order path is not written yet. *no perp order API*: the venue publishes none, so trades there are placed by hand; `manual_only` in the caps row lists the markets. |
| `WS`, `FIX` | `WS`: the adapter sends create, cancel and amend over the venue's websocket trade API while that socket is logged in, and over REST otherwise. `FIX`: the adapter sends orders over a FIX session; none does yet, whatever the venue offers. |
| `private stream` | how the account socket authenticates. `ListenKey` gets a token over REST and puts it in the URL; `AuthSubscribe` signs a frame on an open socket; `–` means there is no account socket, and fills arrive through the reconciliation pull. |
| `funding ledger` | the adapter reads the funding the venue actually charged from the account ledger. Without it, funding PnL is not tracked. |

<!-- venues:begin -->
| venue | custody | markets | order entry | WS | FIX | private stream | funding ledger |
| :-- | :-: | :-- | :-- | :-: | :-: | :-- | :-: |
| <img src="../frontend/packages/ui/src/assets/exchanges/apex.png" height="18" align="absmiddle" alt="">&nbsp;`apex` | DEX | perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/arcus.png" height="18" align="absmiddle" alt="">&nbsp;`arcus` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/aster.png" height="18" align="absmiddle" alt="">&nbsp;`aster` | DEX | spot · perp | REST | – | – | ListenKey | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/backpack.png" height="18" align="absmiddle" alt="">&nbsp;`backpack` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/binance.svg" height="18" align="absmiddle" alt="">&nbsp;`binance` | CEX | spot · perp · options | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/bingx.png" height="18" align="absmiddle" alt="">&nbsp;`bingx` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/bitfinex.png" height="18" align="absmiddle" alt="">&nbsp;`bitfinex` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/bitget.png" height="18" align="absmiddle" alt="">&nbsp;`bitget` | CEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/bithumb.png" height="18" align="absmiddle" alt="">&nbsp;`bithumb` | CEX | spot | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/bitstamp.png" height="18" align="absmiddle" alt="">&nbsp;`bitstamp` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/bitunix.png" height="18" align="absmiddle" alt="">&nbsp;`bitunix` | CEX | spot · perp | REST | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/blofin.png" height="18" align="absmiddle" alt="">&nbsp;`blofin` | CEX | perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/bullish.png" height="18" align="absmiddle" alt="">&nbsp;`bullish` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/bybit.svg" height="18" align="absmiddle" alt="">&nbsp;`bybit` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/coinbase.png" height="18" align="absmiddle" alt="">&nbsp;`coinbase` | CEX | spot | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/cryptocom.png" height="18" align="absmiddle" alt="">&nbsp;`cryptocom` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/deribit.svg" height="18" align="absmiddle" alt="">&nbsp;`deribit` | CEX | spot · perp · futures · options | REST | – | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/derive.png" height="18" align="absmiddle" alt="">&nbsp;`derive` | DEX | perp · options | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/dydx.png" height="18" align="absmiddle" alt="">&nbsp;`dydx` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/edgex.png" height="18" align="absmiddle" alt="">&nbsp;`edgex` | DEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/extended.svg" height="18" align="absmiddle" alt="">&nbsp;`extended` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/gate.svg" height="18" align="absmiddle" alt="">&nbsp;`gate` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/grvt.png" height="18" align="absmiddle" alt="">&nbsp;`grvt` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/htx.png" height="18" align="absmiddle" alt="">&nbsp;`htx` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/hyperliquid.svg" height="18" align="absmiddle" alt="">&nbsp;`hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/kraken.png" height="18" align="absmiddle" alt="">&nbsp;`kraken` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/kucoin.png" height="18" align="absmiddle" alt="">&nbsp;`kucoin` | CEX | spot · perp | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/lbank.png" height="18" align="absmiddle" alt="">&nbsp;`lbank` | CEX | spot · perp | **market data only**, no perp order API | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/lighter.png" height="18" align="absmiddle" alt="">&nbsp;`lighter` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/mexc.png" height="18" align="absmiddle" alt="">&nbsp;`mexc` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/nado.png" height="18" align="absmiddle" alt="">&nbsp;`nado` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/okx.svg" height="18" align="absmiddle" alt="">&nbsp;`okx` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/ondoperps.png" height="18" align="absmiddle" alt="">&nbsp;`ondoperps` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/ostium.png" height="18" align="absmiddle" alt="">&nbsp;`ostium` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/pacifica.png" height="18" align="absmiddle" alt="">&nbsp;`pacifica` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/paradex.png" height="18" align="absmiddle" alt="">&nbsp;`paradex` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/phemex.png" height="18" align="absmiddle" alt="">&nbsp;`phemex` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/polymarketperps.png" height="18" align="absmiddle" alt="">&nbsp;`polymarketperps` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/toobit.png" height="18" align="absmiddle" alt="">&nbsp;`toobit` | CEX | spot · perp | REST | – | – | ListenKey | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/upbit.png" height="18" align="absmiddle" alt="">&nbsp;`upbit` | CEX | spot | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/vest.png" height="18" align="absmiddle" alt="">&nbsp;`vest` | DEX | perp | **market data only** | – | – | – | – |
| <img src="../frontend/packages/ui/src/assets/exchanges/weex.png" height="18" align="absmiddle" alt="">&nbsp;`weex` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="../frontend/packages/ui/src/assets/exchanges/xt.png" height="18" align="absmiddle" alt="">&nbsp;`xt` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
<!-- venues:end -->

<details>
<summary>Adding a venue</summary>

<br>

A market-data adapter is about 3,500 lines: catalogue, symbol map, book, envelope
dispatch, REST builder and websocket client. Order entry adds a signer, an execution
client and the reconciliation pulls.

Capture fixtures from the live venue first, into `tests/venues/<name>/fixtures` with a
README that names each file's endpoint and capture date. Write the parsers against
those bytes, then wire the client.

Register the venue in four places: `VenueId` in `libs/domain`, the caps row, the
factory, and `register_adapter()` in `apps/gatewayd/main.cpp`. Registration is explicit
because with static self-registration the linker can drop a venue from a static library
without a diagnostic. Chart history comes from a kline reader in `libs/candles/src/venues`.

</details>

## Performance

Measured on an Apple M-series laptop with a release build (`make bench`). Production
runs on Linux x86-64.

Shared-memory primitives, single operation, no contention:

| operation         |    time |
| :---------------- | ------: |
| seqlock read      | 0.68 ns |
| SPSC push + pop   | 1.71 ns |
| seqlock write     | 1.72 ns |
| broadcast publish | 2.14 ns |

Cross-process round trip, two processes, one SPSC ring each way, 200,000 round trips:

|       |    RTT | one way |
| :---- | -----: | ------: |
| p50   | 167 ns |   83 ns |
| p90   | 208 ns |  104 ns |
| p99   | 250 ns |  125 ns |
| p99.9 | 292 ns |  146 ns |

The maximum was 19.5 µs, from scheduler preemption; macOS cannot pin a thread to a
core. On Linux the benchmark pins with `--cpu A --cpu-child B`. The daemons do not pin
yet.

## Build and test

Requires CMake ≥ 3.28, Ninja, Conan 2 and a C++23 compiler (GCC 14, Clang 18 or
AppleClang 17).

```sh
make deps                  # dependencies via Conan
make test                  # ASan + UBSan
make test PRESET=dev-tsan  # ThreadSanitizer
make bench                 # Release benchmarks
```

## Releases

raikiri follows [semantic versioning](https://semver.org). Until 1.0 any minor release may change
the API or a shared-memory layout, and its notes will say so; a gateway and a kernel built with
different shared-memory layouts refuse to attach rather than misread each other. The version lives
in [`VERSION`](../VERSION), every binary prints it with `--version`, and
[RELEASES.md](../RELEASES.md) lists what each release changed.

## Status

Working today: market data on all 43 venues in the table, REST order entry on the 24 it
marks, the shared-memory transport, the SBE journals and their Postgres copy, and the
HTTP/WebSocket admin API in [`schema/admin-api.yaml`](../schema/admin-api.yaml).

Not every order path has run against a real account yet, and the caps table does not
record which have.

The API is not stable yet. A release that changes a shared-memory layout will say so.
