<div align="center">

<img src=".github/assets/lightning.gif" alt="raikiri lightning" width="640">

# raikiri

> 雷切 — "lightning cutter", the sword said to have cut a bolt of lightning.
> raikiri is a low-latency, multi-process crypto trading framework in C++23.

[![Release](https://img.shields.io/badge/release-0.1.0--beta.1-orange?style=flat-square)](RELEASES.md)
[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![Latency](https://img.shields.io/badge/cross--process%20p50-167%20ns-2da44e?style=flat-square)](docs/README.md#performance)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-43%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-2700%2B-2da44e?style=flat-square)](docs/README.md#build-and-test)
[![Sanitizers](https://img.shields.io/badge/ASan%20·%20UBSan%20·%20TSan-green?style=flat-square)](docs/README.md#build-and-test)

[Quick start](#quick-start) · [Write a strategy](#write-a-strategy) · [Terminal](#terminal) ·
[Venues](#venues) · [Documentation](docs/README.md) · [Releases](RELEASES.md) ·
[API contract](schema/admin-api.yaml)

</div>

## Introduction

raikiri connects to crypto exchanges, normalises their market data and order flow, and hands both to
your strategy through shared memory. Each exchange runs in its own gateway process; a single-threaded
kernel holds one book across all of them, so a strategy reads every venue it trades and sends orders
back without a socket, a broker or a lock on the way.

It is written for people who build their own trading systems: the framework handles the parts every
desk rewrites (connections, symbol mapping, rate limits, signing, reconciliation, journals) and stays
out of the decision itself. Strategies are plain C++ classes, and the library is LGPL, so they can
stay closed source.

## Features

- **Fast**: cross-process round trip p50 167 ns over SPSC rings; seqlock reads in under a nanosecond;
  no allocation on the hot path.
- **Many venues**: 43 CEX and DEX venues for market data, 23 with order entry, across spot, perpetuals,
  futures and options.
- **Exact**: money is fixed-point `int64` end to end, and a value off the venue's tick or lot grid is
  refused, never rounded.
- **Isolated**: one process per venue and market; a crashed or banned gateway takes nothing else down.
- **Recorded**: every process writes an append-only SBE journal, copied into PostgreSQL off the
  trading path.
- **Safe with keys**: credentials are references (`env:`, `file:`, `exec:`, `op://`), never values on
  a command line; a risk gate checks every order against notional, band and open-order limits.
- **Execution algos**: `joe` works parent orders across venues as TWAP, VWAP or smart-routed slices.
- **Observable**: an HTTP and WebSocket admin API, Prometheus metrics, and a web terminal on top.

## Terminal

<!-- terminal demo: record a short GIF and save it as .github/assets/terminal.gif, then uncomment
<p align="center">
  <img src=".github/assets/terminal.gif" alt="raikiri terminal" width="900">
</p>
-->

The web terminal in [`frontend/`](frontend/README.md) runs on the admin API:

- **Markets**: market watch, chart, order book, an order ticket and the blotter.
- **Scanner**: tables from any board service plugged into admind with `--upstream boards=<port>`.
- **Execution**: parent orders worked by `joe`, their child orders, and a ticket to start one.
- **Treasury**: balances, positions and P&L split into realised, fees and funding, by venue and over
  time.
- **Settings**: the accounts admind knows.

`make up` starts gateways, admind and the terminal locally.

## Quick start

CMake ≥ 3.28, Ninja, Conan 2 and a C++23 compiler (GCC 14, Clang 18 or AppleClang 17).

```sh
git clone https://github.com/TeachPenguinsFly/raikiri.git
cd raikiri
make deps && make build
```

Watch a live market; public data needs no keys:

```sh
build/dev-asan/apps/gatewayd/gatewayd --venue=bybit --symbol=BTCUSDT --duration 20
```

Run the whole pipeline against the simulated venue, with a latency table at the end:

```sh
make demo
```

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

Besides `on_bbo` there are `on_trade`, `on_funding`, `on_order_event`, `on_timer`, `on_start` and
`on_stop`. `Context` also gives the strategy its position, unrealised PnL, timers and `set_target`,
which declares the position to hold and lets the kernel send the orders.
[`demo_maker.hpp`](libs/strategies/include/raikiri/strategies/demo_maker.hpp) is a complete two-sided
example.

## Architecture

```mermaid
flowchart LR
    X["exchanges"] -- "WS · REST" --> G["gatewayd<br/>one per venue and market"]
    G -- "shared memory" --> K["strategyd"]
    K -- "orders" --> G
    G & K --> J["journals"] --> P[("PostgreSQL")]
    G & P --> A["admind"] --> T(["terminal"])
```

Each gateway owns a shared-memory segment; the kernel reads all of them, keeps one book and sends
orders back through lossless rings. The [documentation](docs/README.md#architecture) covers the
channels, the invariants the code relies on, running a gateway, configuration and adding a venue.

## Venues

`custody` says who holds the coins: the exchange (CEX) or a contract (DEX). `order entry` is the
transport orders take; *market data only* means the adapter sends none yet. The
[venue table notes](docs/README.md#venue-table) explain every column.

<!-- venues:begin -->
| venue | custody | markets | order entry | WS | FIX | private stream | funding ledger |
| :-- | :-: | :-- | :-- | :-: | :-: | :-- | :-: |
| <img src="frontend/src/assets/exchanges/apex.png" height="18" align="absmiddle" alt="">&nbsp;`apex` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/arcus.png" height="18" align="absmiddle" alt="">&nbsp;`arcus` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/aster.png" height="18" align="absmiddle" alt="">&nbsp;`aster` | DEX | spot · perp | REST | – | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/backpack.png" height="18" align="absmiddle" alt="">&nbsp;`backpack` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/binance.svg" height="18" align="absmiddle" alt="">&nbsp;`binance` | CEX | spot · perp · options | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/bingx.png" height="18" align="absmiddle" alt="">&nbsp;`bingx` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/bitfinex.png" height="18" align="absmiddle" alt="">&nbsp;`bitfinex` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/bitget.png" height="18" align="absmiddle" alt="">&nbsp;`bitget` | CEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/bithumb.png" height="18" align="absmiddle" alt="">&nbsp;`bithumb` | CEX | spot | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/bitstamp.png" height="18" align="absmiddle" alt="">&nbsp;`bitstamp` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/bitunix.png" height="18" align="absmiddle" alt="">&nbsp;`bitunix` | CEX | spot · perp | REST | – | – | – | – |
| <img src="frontend/src/assets/exchanges/blofin.png" height="18" align="absmiddle" alt="">&nbsp;`blofin` | CEX | perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/bullish.png" height="18" align="absmiddle" alt="">&nbsp;`bullish` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/bybit.svg" height="18" align="absmiddle" alt="">&nbsp;`bybit` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/coinbase.png" height="18" align="absmiddle" alt="">&nbsp;`coinbase` | CEX | spot | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/cryptocom.png" height="18" align="absmiddle" alt="">&nbsp;`cryptocom` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/deribit.svg" height="18" align="absmiddle" alt="">&nbsp;`deribit` | CEX | spot · perp · futures · options | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/derive.png" height="18" align="absmiddle" alt="">&nbsp;`derive` | DEX | perp · options | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/dydx.png" height="18" align="absmiddle" alt="">&nbsp;`dydx` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/edgex.png" height="18" align="absmiddle" alt="">&nbsp;`edgex` | DEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/extended.svg" height="18" align="absmiddle" alt="">&nbsp;`extended` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/gate.svg" height="18" align="absmiddle" alt="">&nbsp;`gate` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/grvt.png" height="18" align="absmiddle" alt="">&nbsp;`grvt` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/htx.png" height="18" align="absmiddle" alt="">&nbsp;`htx` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/hyperliquid.svg" height="18" align="absmiddle" alt="">&nbsp;`hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/kraken.png" height="18" align="absmiddle" alt="">&nbsp;`kraken` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/kucoin.png" height="18" align="absmiddle" alt="">&nbsp;`kucoin` | CEX | spot · perp | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/lbank.png" height="18" align="absmiddle" alt="">&nbsp;`lbank` | CEX | spot · perp | **market data only**, no perp order API | – | – | – | – |
| <img src="frontend/src/assets/exchanges/lighter.png" height="18" align="absmiddle" alt="">&nbsp;`lighter` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/mexc.png" height="18" align="absmiddle" alt="">&nbsp;`mexc` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/nado.png" height="18" align="absmiddle" alt="">&nbsp;`nado` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/okx.svg" height="18" align="absmiddle" alt="">&nbsp;`okx` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/ondoperps.png" height="18" align="absmiddle" alt="">&nbsp;`ondoperps` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/ostium.png" height="18" align="absmiddle" alt="">&nbsp;`ostium` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/pacifica.png" height="18" align="absmiddle" alt="">&nbsp;`pacifica` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/paradex.png" height="18" align="absmiddle" alt="">&nbsp;`paradex` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/phemex.png" height="18" align="absmiddle" alt="">&nbsp;`phemex` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/polymarketperps.png" height="18" align="absmiddle" alt="">&nbsp;`polymarketperps` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/toobit.png" height="18" align="absmiddle" alt="">&nbsp;`toobit` | CEX | spot · perp | REST | – | – | ListenKey | – |
| <img src="frontend/src/assets/exchanges/upbit.png" height="18" align="absmiddle" alt="">&nbsp;`upbit` | CEX | spot | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/vest.png" height="18" align="absmiddle" alt="">&nbsp;`vest` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/weex.png" height="18" align="absmiddle" alt="">&nbsp;`weex` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/xt.png" height="18" align="absmiddle" alt="">&nbsp;`xt` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
<!-- venues:end -->

## Releases

raikiri follows [semantic versioning](https://semver.org). Until 1.0 any minor release may change
the API or a shared-memory layout, and its notes will say so; a gateway and a kernel built with different
shared-memory layouts refuse to attach rather than misread each other. The version lives in [`VERSION`](VERSION),
every binary prints it with `--version`, and [RELEASES.md](RELEASES.md) lists what each release
changed.

The current release is **0.1.0-beta.1**: the first public beta. Market data is in daily use; order
entry is tested against recorded venue replies, and not every order path has run against a real
account yet.

## Disclaimer

raikiri is software for building trading systems, not a trading system. It comes with no warranty,
and trading with it can lose money, fast. Test against the simulated venue and with small size
first; you are responsible for complying with each venue's terms and your local law.

## License

LGPL-3.0, see [`LICENSE`](LICENSE). [`COPYING`](COPYING) holds the GPL-3.0 text it
incorporates by reference. A strategy that links raikiri can stay closed source; a distributed
modification of raikiri itself must publish its changes under the same license.
