<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/assets/wordmark-dark.svg">
    <img src=".github/assets/wordmark-light.svg" alt="raikiri" width="400">
  </picture>
</p>

<p align="center">
  Open-source trading for crypto exchanges, centralised and on-chain.<br>
  Live market data, order execution and a terminal for every venue, in one place.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square" alt="LGPL-3.0"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square" alt="C++23">
  <a href="https://github.com/TeachPenguinsFly/raikiri/actions/workflows/build.yml"><img src="https://github.com/TeachPenguinsFly/raikiri/actions/workflows/build.yml/badge.svg" alt="build"></a>
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#venues">Venues</a> ·
  <a href="ARCHITECTURE.md">How it works</a> ·
  <a href="docs/README.md">Documentation</a>
</p>

<p align="center">
  <img src=".github/assets/terminal.png" alt="The raikiri terminal: one market's chart with every venue's price, the venues side by side and the order book" width="100%">
</p>

Most trading desks build the same plumbing before they can test an idea: exchange connections,
symbol mapping, rate limits, order signing, bookkeeping. raikiri ships all of it, so you write only
the strategy. A strategy is a plain C++ class, and it can stay closed source.

## Features

### All venues on one screen

The terminal draws every venue's price for a market on one chart and lists their quotes side by
side, so you can see where a coin is cheaper. Beside them, the order book shows one venue's depth
or all of them merged.

### Every market in one list

Hover the market's name and every market opens, in spot, perpetual and options tabs, each coin
with its name and icon. Coins listed on several venues come first, then the rest by 24-hour
volume, so thin look-alike tickers end up at the bottom.

<p align="center">
  <img src=".github/assets/markets.png" alt="The market list with spot, perpetual and options tabs" width="560">
</p>

### Charts with history

`candled` folds every trade the gateways see into one-minute candles in PostgreSQL. Whatever it
missed, it fetches from each venue's public history, so a chart opens with days of bars, from one
minute to one day.

### Options from six exchanges

The options desk merges the chains of Deribit, OKX, Bybit, Binance, Gate and Derive, and shows
the volatility smile and term structure beside them.

<p align="center">
  <img src=".github/assets/options.png" alt="The options desk" width="100%">
</p>

### Smart order routing

A strategy can hand a large order to the router, which splits it across venues by price after
fees. No part of it goes deeper into a book than planned. TWAP and VWAP are there too.

<p align="center">
  <img src=".github/assets/routing.svg" alt="One order split across four venues" width="100%">
</p>

### Very fast

A price reaches your strategy in under a microsecond (gateways and strategies run on one machine
and share memory, so nothing goes over a network).

### Safe by default

Every order passes a risk check, and API keys never appear on a command line. Each venue gets a
request budget below its rate limit, so you don't get banned.

### Open to your own tools

Programs on other machines trade through the Python and C++ SDKs. There is also a web API with an
OpenAPI spec, and Prometheus metrics.

## Quick start

```sh
git clone https://github.com/TeachPenguinsFly/raikiri.git
cd raikiri
make deps && make build
make up
```

The terminal opens at <http://127.0.0.1:5173>. Market data needs no API keys. You need CMake,
Ninja, Conan 2 and a C++23 compiler. To keep chart history, run `make dup` instead, which also
starts PostgreSQL in Docker, and `make web` beside it for the terminal.

## Venues

<!-- venues:begin -->
43 venues. Spot markets on 29, perpetual futures (perp) on 40, dated futures on 1 and options on 6.

### Trading and market data

<table>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/apex.png" width="28" height="28" alt=""><br><b>ApeX</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/aster.png" width="28" height="28" alt=""><br><b>Aster</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/binance.svg" width="28" height="28" alt=""><br><b>Binance</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bingx.png" width="28" height="28" alt=""><br><b>BingX</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bitfinex.png" width="28" height="28" alt=""><br><b>Bitfinex</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bitget.png" width="28" height="28" alt=""><br><b>Bitget</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bitunix.png" width="28" height="28" alt=""><br><b>Bitunix</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/blofin.png" width="28" height="28" alt=""><br><b>BloFin</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bybit.svg" width="28" height="28" alt=""><br><b>Bybit</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/coinbase.png" width="28" height="28" alt=""><br><b>Coinbase</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/deribit.svg" width="28" height="28" alt=""><br><b>Deribit</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-futures.svg" height="16" alt="futures"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/edgex.png" width="28" height="28" alt=""><br><b>edgeX</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/gate.svg" width="28" height="28" alt=""><br><b>Gate</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/htx.png" width="28" height="28" alt=""><br><b>HTX</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/hyperliquid.svg" width="28" height="28" alt=""><br><b>Hyperliquid</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/kraken.png" width="28" height="28" alt=""><br><b>Kraken</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/kucoin.png" width="28" height="28" alt=""><br><b>KuCoin</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/lighter.png" width="28" height="28" alt=""><br><b>Lighter</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/mexc.png" width="28" height="28" alt=""><br><b>MEXC</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/okx.svg" width="28" height="28" alt=""><br><b>OKX</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/phemex.png" width="28" height="28" alt=""><br><b>Phemex</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/toobit.png" width="28" height="28" alt=""><br><b>Toobit</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/weex.png" width="28" height="28" alt=""><br><b>WEEX</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/xt.png" width="28" height="28" alt=""><br><b>XT</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
</table>

### Market data only

<table>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/arcus.png" width="28" height="28" alt=""><br><b>Arcus</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/backpack.png" width="28" height="28" alt=""><br><b>Backpack</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bithumb.png" width="28" height="28" alt=""><br><b>Bithumb</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bitstamp.png" width="28" height="28" alt=""><br><b>Bitstamp</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/bullish.png" width="28" height="28" alt=""><br><b>Bullish</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/cryptocom.png" width="28" height="28" alt=""><br><b>Crypto.com</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/derive.png" width="28" height="28" alt=""><br><b>Derive</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"> <img src=".github/assets/market-options.svg" height="16" alt="options"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/dydx.png" width="28" height="28" alt=""><br><b>dYdX</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/extended.svg" width="28" height="28" alt=""><br><b>Extended</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/grvt.png" width="28" height="28" alt=""><br><b>GRVT</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/lbank.png" width="28" height="28" alt=""><br><b>LBank</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"> <img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/nado.png" width="28" height="28" alt=""><br><b>Nado</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/ondoperps.png" width="28" height="28" alt=""><br><b>Ondo</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/ostium.png" width="28" height="28" alt=""><br><b>Ostium</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/pacifica.png" width="28" height="28" alt=""><br><b>Pacifica</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/paradex.png" width="28" height="28" alt=""><br><b>Paradex</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/polymarketperps.png" width="28" height="28" alt=""><br><b>Polymarket</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/upbit.png" width="28" height="28" alt=""><br><b>Upbit</b><br><img src=".github/assets/market-spot.svg" height="16" alt="spot"></td></tr>
<tr><td align="center" width="140"><img src="frontend/packages/ui/src/assets/exchanges/vest.png" width="28" height="28" alt=""><br><b>Vest</b><br><img src=".github/assets/market-perp.svg" height="16" alt="perp"></td></tr>
</table>
<!-- venues:end -->

[Everything each venue supports](docs/README.md#venue-table)

## Learn more

- [How it works](ARCHITECTURE.md): processes, shared memory, the order path, speed
- [Documentation](docs/README.md): writing a strategy, the client API, running gateways, configuration, adding a venue
- [Client SDKs](sdk/python/README.md): Python and C++, with [examples](examples/README.md)

## License

LGPL-3.0, see [`LICENSE`](LICENSE). Your strategy can link raikiri and stay closed source. If you
distribute changes to raikiri itself, you publish them under the same license.

raikiri comes with no warranty, and trading can lose money. Start on the simulated venue with
small size, and follow each venue's terms and your local law.
