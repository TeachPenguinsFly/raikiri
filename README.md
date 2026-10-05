<div align="center">

<img src=".github/assets/lightning.gif" alt="raikiri lightning" width="640">

# raikiri

A low-latency, multi-process crypto trading framework in C++23

[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![Latency](https://img.shields.io/badge/cross--process%20p50-167%20ns-2da44e?style=flat-square)](docs/README.md#performance)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-32%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-2300-2da44e?style=flat-square)](docs/README.md#build-and-test)
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
| | venue | custody | markets | order entry | WS | FIX | private stream | funding ledger |
| :-: | :-- | :-: | :-- | :-- | :-: | :-: | :-- | :-: |
| <img src="frontend/src/assets/exchanges/apex.png" width="20" height="20" alt="apex"> | `apex` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/aster.png" width="20" height="20" alt="aster"> | `aster` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/backpack.png" width="20" height="20" alt="backpack"> | `backpack` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/binance.svg" width="20" height="20" alt="binance"> | `binance` | CEX | spot · perp · options | REST + WS | ✓ | ✓ | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/bingx.png" width="20" height="20" alt="bingx"> | `bingx` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/bitfinex.png" width="20" height="20" alt="bitfinex"> | `bitfinex` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/bitget.png" width="20" height="20" alt="bitget"> | `bitget` | CEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/bitunix.png" width="20" height="20" alt="bitunix"> | `bitunix` | CEX | spot · perp | REST | – | – | – | – |
| <img src="frontend/src/assets/exchanges/blofin.png" width="20" height="20" alt="blofin"> | `blofin` | CEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/bybit.svg" width="20" height="20" alt="bybit"> | `bybit` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/coinbase.png" width="20" height="20" alt="coinbase"> | `coinbase` | CEX | spot | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/cryptocom.png" width="20" height="20" alt="cryptocom"> | `cryptocom` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/deribit.svg" width="20" height="20" alt="deribit"> | `deribit` | CEX | perp · futures · options | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/derive.png" width="20" height="20" alt="derive"> | `derive` | DEX | perp · options | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/dydx.png" width="20" height="20" alt="dydx"> | `dydx` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/edgex.png" width="20" height="20" alt="edgex"> | `edgex` | DEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/extended.svg" width="20" height="20" alt="extended"> | `extended` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/gate.svg" width="20" height="20" alt="gate"> | `gate` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/grvt.png" width="20" height="20" alt="grvt"> | `grvt` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/htx.png" width="20" height="20" alt="htx"> | `htx` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/hyperliquid.svg" width="20" height="20" alt="hyperliquid"> | `hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/kraken.png" width="20" height="20" alt="kraken"> | `kraken` | CEX | spot · perp | REST | – | – | AuthSubscribe | – |
| <img src="frontend/src/assets/exchanges/kucoin.png" width="20" height="20" alt="kucoin"> | `kucoin` | CEX | spot · perp | REST + WS | ✓ | – | ListenKey | ✓ |
| <img src="frontend/src/assets/exchanges/lbank.png" width="20" height="20" alt="lbank"> | `lbank` | CEX | spot · perp | **market data only**, no perp order API | – | – | – | – |
| <img src="frontend/src/assets/exchanges/lighter.png" width="20" height="20" alt="lighter"> | `lighter` | DEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/mexc.png" width="20" height="20" alt="mexc"> | `mexc` | CEX | spot · perp | REST | – | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/okx.svg" width="20" height="20" alt="okx"> | `okx` | CEX | spot · perp · options | REST + WS | ✓ | – | AuthSubscribe | ✓ |
| <img src="frontend/src/assets/exchanges/pacifica.png" width="20" height="20" alt="pacifica"> | `pacifica` | DEX | perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/phemex.png" width="20" height="20" alt="phemex"> | `phemex` | CEX | spot · perp | **market data only** | – | – | – | – |
| <img src="frontend/src/assets/exchanges/toobit.png" width="20" height="20" alt="toobit"> | `toobit` | CEX | spot · perp | REST | – | – | ListenKey | – |
| <img src="frontend/src/assets/exchanges/weex.png" width="20" height="20" alt="weex"> | `weex` | CEX | perp | REST | – | – | – | ✓ |
| <img src="frontend/src/assets/exchanges/xt.png" width="20" height="20" alt="xt"> | `xt` | CEX | spot · perp | REST | – | – | ListenKey | ✓ |
<!-- venues:end -->

## License

LGPL-3.0, see [`LICENSE`](LICENSE). [`COPYING`](COPYING) holds the GPL-3.0 text it
incorporates by reference. A strategy that links raikiri can stay closed source; a distributed
modification of raikiri itself must publish its changes under the same license.
