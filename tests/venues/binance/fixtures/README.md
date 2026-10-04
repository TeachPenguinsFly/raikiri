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

## WebSocket API order replies

No order was placed to get these. The four doc examples are copied from
binance's USD-M futures WebSocket API pages, as published on 2026-10-03.
The general page is
`https://developers.binance.com/docs/derivatives/usds-margined-futures/websocket-api-general-info`
and the trade page is
`https://developers.binance.com/docs/derivatives/usds-margined-futures/trade/websocket-api`.
Their ids are the docs' uuids, so a test swaps in a numeric id.

| file | source |
|---|---|
| `ws_api_order_place.json` | doc example: the `order.place` success reply on the general page, with all three `rateLimits` rows |
| `ws_api_order_cancel.json` | doc example: the `order.cancel` success reply on the trade page |
| `ws_api_order_modify.json` | doc example: the `order.modify` success reply on the trade page |
| `ws_api_error.json` | doc example: the error reply on the general page, code -1102 |
| `ws_api_auth_failure.json` | assembled. The docs give no WebSocket auth failure, so this is the documented error envelope carrying -1022 with its text from the error-code page |

## Spot

Captured live from production binance spot on 2026-10-04, REST on
`https://api.binance.com` and the socket on `wss://stream.binance.com:443/stream`.
Public endpoints, no credentials.

| file | source |
|---|---|
| `spot_ws_book_ticker.json` | `btcusdt@bookTicker`; spot's frame has no `e`, `E` or `T` |
| `spot_ws_trade.json` | `btcusdt@trade` |
| `spot_rest_depth_seed.json`, `spot_ws_depth_contained.json`, `spot_ws_depth_update.json`, `spot_ws_depth_update_next.json` | one capture, described below |
| `spot_rest_exchange_info.json` | `GET /api/v3/exchangeInfo?showPermissionSets=false`, trimmed to four symbols: BTCUSDT, ETHBTC, NEOBTC (`BREAK`) and 币安人生USDT, a pair whose base is not ascii |
| `spot_rest_ticker_24h.json` | `GET /api/v3/ticker/24hr`, trimmed to BTCUSDT, ETHBTC and NEOBTC |

The socket on `btcusdt@depth@100ms` was recorded first, and
`GET /api/v3/depth?symbol=BTCUSDT&limit=100` was issued three seconds in.
Its `lastUpdateId` is 101022247332. `spot_ws_depth_contained.json` ends on that
id, `spot_ws_depth_update.json` starts on the next one, and
`spot_ws_depth_update_next.json` follows it. Spot ids run contiguous and the
frames carry no `pu`.
