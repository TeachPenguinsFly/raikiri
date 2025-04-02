# binance fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Captured live from production binance USD-M futures (2025-04, no credentials
required — all of these are public endpoints):

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.binance.com/stream?streams=btcusdt@bookTicker` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two consecutive frames so the `pu`→`u` chain is testable |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed from 858 symbols to four |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_server_time.json` | `GET /fapi/v1/time` |
| `rest_error_bad_symbol.json` | `GET /fapi/v1/ticker/bookTicker?symbol=NOSUCHSYM` |
| `rest_error_bad_apikey.json` | `GET /fapi/v1/order?symbol=NOPE` |

Assembled, not captured:

| file | how |
|---|---|
| `ws_agg_trade.json` | numbers are a real trade from `GET /fapi/v1/aggTrades?limit=1`; the `e`/`E`/`s` envelope is binance's documented `aggTrade` wrapper |
| `ws_mark_price.json` | numbers are a real `premiumIndex` response; the envelope is the documented `markPriceUpdate` wrapper |

The assembled two are honest about their field values but were not observed on
the socket, so they prove the parser reads the documented shape — not that the
shape is what production sends. Replace them with captures when a recorder
exists.

`rest_exchange_info.json` keeps four symbols on purpose: BTCUSDT and ETHUSDT
(`PERPETUAL`/`TRADING`, both loadable), XAUUSDT (`TRADIFI_PERPETUAL` — a
contract type the perp loader must skip) and OMGUSDT (`SETTLING` — a status it
must skip). A loader that accepts all four is wrong in a way that only shows
up months later, on a delisting.
