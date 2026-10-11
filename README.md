<div align="center">

<img src=".github/assets/lightning.gif" alt="raikiri lightning" width="640">

# raikiri

A low-latency, multi-process crypto trading framework in C++23

[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![Latency](https://img.shields.io/badge/cross--process%20p50-167%20ns-2da44e?style=flat-square)](docs/README.md#performance)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-42%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-2500%2B-2da44e?style=flat-square)](docs/README.md#build-and-test)
[![Sanitizers](https://img.shields.io/badge/ASan%20·%20UBSan%20·%20TSan-green?style=flat-square)](docs/README.md#build-and-test)

[Documentation](docs/README.md) · [Venues](#venues) ·
[API contract](schema/admin-api.yaml)

</div>

## Install

CMake ≥ 3.28, Ninja, Conan 2 and a C++23 compiler (GCC 14, Clang 18 or AppleClang 17).

```sh
git clone https://github.com/TeachPenguinsFly/raikiri.git
cd raikiri
make deps && make build
```

To watch a live market:

```sh
build/dev-asan/apps/gatewayd/gatewayd --venue=bybit --symbol=BTCUSDT --duration 20
```

The [documentation](docs/README.md) covers the architecture, the shared-memory transport, running a gateway and adding a venue.

## Venues

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
| <img src="frontend/src/assets/exchanges/edgex.png" height="18" align="absmiddle" alt="">&nbsp;`edgex` | DEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/extended.svg" height="18" align="absmiddle" alt="">&nbsp;`extended` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/gate.svg" height="18" align="absmiddle" alt="">&nbsp;`gate` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/grvt.png" height="18" align="absmiddle" alt="">&nbsp;`grvt` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/htx.png" height="18" align="absmiddle" alt="">&nbsp;`htx` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/hyperliquid.svg" height="18" align="absmiddle" alt="">&nbsp;`hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/kraken.png" height="18" align="absmiddle" alt="">&nbsp;`kraken` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/kucoin.png" height="18" align="absmiddle" alt="">&nbsp;`kucoin` | CEX | spot · perp | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/lbank.png" height="18" align="absmiddle" alt="">&nbsp;`lbank` | CEX | spot · perp | **market data only**, no perp order API | – | – | – | – |
| <img src="frontend/src/assets/exchanges/lighter.png" height="18" align="absmiddle" alt="">&nbsp;`lighter` | DEX | spot · perp | REST + WS (perp) | ✓ | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/mexc.png" height="18" align="absmiddle" alt="">&nbsp;`mexc` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
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

## License

LGPL-3.0, see [`LICENSE`](LICENSE). [`COPYING`](COPYING) holds the GPL-3.0 text it
incorporates by reference. A strategy that links raikiri can stay closed source; a distributed
modification of raikiri itself must publish its changes under the same license.
