<div align="center">

# raikiri

**A low-latency, multi-process crypto trading framework in C++23**

[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![Latency](https://img.shields.io/badge/cross--process%20p50-167%20ns-2da44e?style=flat-square)](docs/README.md#performance)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-19%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-1755-2da44e?style=flat-square)](docs/README.md#build-and-test)
[![Sanitizers](https://img.shields.io/badge/ASan%20·%20UBSan%20·%20TSan-green?style=flat-square)](docs/README.md#build-and-test)

[**Documentation**](docs/README.md) · [Venues](#venues) ·
[API contract](schema/admin-api.yaml)

</div>

---

## Install

CMake ≥ 3.28, Ninja, Conan 2, and a C++23 compiler — GCC 14, Clang 18 or
AppleClang 17.

```sh
git clone https://github.com/TeachPenguinsFly/raikiri.git
cd raikiri
make deps && make build
```

Watch a live market. No credentials — every adapter's public path runs
unauthenticated:

```sh
build/dev-asan/apps/gatewayd/gatewayd --venue=bybit --symbol=BTCUSDT --duration 20
```

**[Documentation →](docs/README.md)** — architecture, the shared-memory transport,
running a gateway, adding a venue.

---

## Venues

<!-- venues:begin -->
| venue | custody | markets | order entry | WS | FIX | amend | private stream | funding ledger |
| :-- | :-: | :-- | :-- | :-: | :-: | :-: | :-- | :-: |
| `aster` | DEX | perp | **market data only** | – | – | – | – | – |
| `backpack` | CEX | spot · perp | **market data only** | – | – | – | – | – |
| `binance` | CEX | perp | REST + WS | ✓ | ✓ | ✓ | ListenKey | ✓ |
| `bingx` | CEX | spot · perp | REST | – | – | – | ListenKey | ✓ |
| `bitfinex` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `bitget` | CEX | spot · perp | REST + WS | ✓ | – | – | AuthSubscribe | ✓ |
| `bybit` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `coinbase` | CEX | spot | REST | – | – | – | AuthSubscribe | – |
| `cryptocom` | CEX | spot · perp | **market data only** | – | – | – | – | – |
| `deribit` | CEX | perp · futures · options | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `gate` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `grvt` | DEX | perp | **market data only** | – | – | – | – | – |
| `htx` | CEX | spot · perp | REST | – | – | – | AuthSubscribe | ✓ |
| `hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `kraken` | CEX | perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `kucoin` | CEX | spot · perp | REST + WS | ✓ | – | – | ListenKey | – |
| `lighter` | DEX | spot · perp | **market data only** | – | – | – | – | – |
| `mexc` | CEX | spot · perp | REST | – | – | – | AuthSubscribe | ✓ |
| `okx` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `weex` | CEX | perp | REST | – | – | – | – | ✓ |
| `xt` | CEX | spot · perp | REST | – | – | ✓ | ListenKey | ✓ |
<!-- venues:end -->

Generated from [`libs/exec/src/caps.cpp`](libs/exec/src/caps.cpp) by
[`tools/venue_table.py`](tools/venue_table.py). Column meanings are in the
[documentation](docs/README.md#venue-table).

---

## License

**LGPL-3.0** — [`LICENSE`](LICENSE), with [`COPYING`](COPYING) holding the GPL-3.0
text it incorporates by reference. A strategy that links raikiri stays closed if its
author wants it closed; a fork of the framework itself publishes its changes.

## Contact Us
