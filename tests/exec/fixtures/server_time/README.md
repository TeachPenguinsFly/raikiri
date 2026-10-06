# server-time fixtures

One body per venue, as its clock probe reads it. Public endpoints, no credentials.
The rows below are the ones captured on 2026-10-06.

| file | request |
|---|---|
| `grvt.json` | `GET https://market-data.grvt.io/time` |
| `hyperliquid.json` | `POST https://api.hyperliquid.xyz/info` with `{"type":"exchangeStatus"}` |
| `blofin.json` | `GET https://openapi.blofin.com/api/v1/market/books?instId=BTC-USDT&size=1` |
| `cryptocom.json` | `GET https://api.crypto.com/exchange/v1/public/get-book?instrument_name=BTCUSD-PERP&depth=1` |
| `upbit.json` | `GET https://api.upbit.com/v1/orderbook?markets=KRW-BTC&count=1` |
| `bithumb.json` | `GET https://api.bithumb.com/v1/orderbook?markets=KRW-BTC&count=1` |
| `bitstamp.json` | `GET https://www.bitstamp.net/api/v2/ticker/btcusd/`, stamped to the second |

Blofin, Crypto.com, Upbit and Bithumb publish no time endpoint, so their probe reads the stamp on the
BTC book. That stamp trails the server clock by the gap since the book's last update,
which was a few milliseconds when these were captured.
