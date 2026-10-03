# binance fixtures

Captured live from production binance USD-M futures, REST on
`https://fapi.binance.com` and the socket on `wss://fstream.binance.com/stream`.
All public endpoints, no credentials. Captured 2025-04 unless a row says
otherwise.

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.binance.com/stream?streams=btcusdt@bookTicker` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two consecutive frames |
| `ws_trade.json` | `btcusdt@trade` (2026-08-27, from the frame's `E`) |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed to four symbols: BTCUSDT, ETHUSDT, XAUUSDT (`TRADIFI_PERPETUAL`) and OMGUSDT (`SETTLING`) |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_depth_seed.json`, `ws_depth_straddle.json`, `ws_depth_straddle_next.json` | one capture (2026-08), described below |
| `rest_server_time.json` | `GET /fapi/v1/time` |
| `rest_income.json` | `GET /fapi/v1/income` (2026-09-16), one COMMISSION row and one FUNDING_FEE row; the shape is live, the figures were replaced |
| `rest_funding_info.json` | `GET /fapi/v1/fundingInfo` (2026-09), trimmed to one row each for the 8h, 4h and 1h intervals |
| `rest_error_bad_symbol.json` | `GET /fapi/v1/ticker/bookTicker?symbol=NOSUCHSYM` |
| `rest_error_bad_apikey.json` | `GET /fapi/v1/order?symbol=NOPE` |

Three files were assembled rather than captured:

| file | how |
|---|---|
| `ws_agg_trade.json` | the numbers are a real trade from `GET /fapi/v1/aggTrades?limit=1`, inside binance's documented `aggTrade` envelope |
| `ws_mark_price.json` | the numbers are a real `premiumIndex` response, inside the documented `markPriceUpdate` envelope |
| `rest_account.json` | `GET /fapi/v2/account` is signed, so this is binance's documented shape and field names with constructed values: twenty asset rows, `multiAssetsMargin` and the `positions` array |

`rest_depth_seed.json`, `ws_depth_straddle.json` and
`ws_depth_straddle_next.json` are one capture. The socket on
`btcusdt@depth@100ms` was recorded first, and
`GET /fapi/v1/depth?symbol=BTCUSDT&limit=100` was issued two seconds in. The two
frames kept are the one whose `[U, u]` contains that snapshot's `lastUpdateId`
and the one right after it. To regenerate them, take the seed and the stream
from the same session in that order.
