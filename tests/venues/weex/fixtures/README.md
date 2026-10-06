# WEEX fixtures

Public files were captured from production WEEX on 2026-09-07, except where a
row gives another date. They are verbatim response bodies and websocket frames,
each written as one line; the two edited captures say so. Private success
bodies are WEEX's own documented examples, since no WEEX credential has been
used here. The private refusals are real captures, taken unauthenticated on
2026-09-13.

Everything is the USDT-margined contract API:

- REST: `https://api-contract.weex.com` (the `/capi/v3/...` tree), plain `curl`,
  no credentials for public paths.
- WS: `wss://ws-contract.weex.com/v3/ws/public`, subscribed with
  `{"method":"SUBSCRIBE","params":[...],"id":1}`. Use a raw-socket script
  rather than `websocat`: `@depth200` frames cross the fragmentation boundary,
  and the server sends `{"event":"ping"}` about every 30 s and expects
  `{"method":"PONG","id":1}` back.

## REST

| file | source |
| --- | --- |
| `rest_error_param_empty.json` | `GET /capi/v3/market/depth` with no parameters, HTTP 400 |
| `rest_error_invalid_param.json` | `GET /capi/v3/market/depth?symbol=BTCUSDT&limit=9999`, HTTP 400 |
| `rest_error_not_found.json` | `GET /capi/v3/market/nope`, HTTP 404 |
| `rest_exchange_info.json` | `GET /capi/v3/market/exchangeInfo`, edited: eight rows (`BTCUSDT`, `DOGEUSDT`, `EURUSDT`, `ONGUSDT`, `THEUSDT`, `USDCUSDT`, `GER40USDT`, `QUBICUSDT`) sliced out by brace matching without re-serialising, the envelope rebuilt as `{"symbols":[…]}`, and `assets` and `rateLimits` dropped |
| `rest_premium_index.json` | `GET /capi/v3/market/premiumIndex`, edited: four rows (`BTCUSDT`, `USDCUSDT`, `GER40USDT`, `THEUSDT`) sliced out the same way and the array rebuilt |
| `rest_api_trading_symbols.json` | `GET /capi/v3/market/apiTradingSymbols`, trimmed to five names; capture date not recorded |

## WebSocket

The depth, trade and ticker files come from one 75 s session subscribed to
`BTCUSDT@depth15`, `ETHUSDT@depth15`, `SOLUSDT@depth15`, `DOGEUSDT@depth15`,
`BTCUSDT@trade`, `ETHUSDT@trade`, `DOGEUSDT@trade`, `BTCUSDT@ticker`,
`ETHUSDT@ticker` and `DOGEUSDT@ticker`.

| file | source |
| --- | --- |
| `ws_connected.json` | the greeting sent on open, before any subscribe |
| `ws_ping.json` | the server ping, `{"event":"ping","time":"…"}` |
| `ws_subscribe_ack.json` | the acknowledgement of a subscribe |
| `ws_subscribe_refused.json` | `params:["BTCUSDT@bookTicker"]` on a fresh socket |
| `ws_depth_snapshot.json` | the first frame of `BTCUSDT@depth15` (`e:"depthSnapshot"`) |
| `ws_depth_update.json`, `ws_depth_update_next.json` | the next two consecutive frames of the same channel |
| `ws_depth200_snapshot.json`, `ws_depth200_update.json` | `BTCUSDT@depth200` on 2026-09-26: the opening snapshot and the delta after it |
| `ws_trade_buy.json` | `BTCUSDT@trade`, prints with `m: false` |
| `ws_trade_sell.json` | `DOGEUSDT@trade`, prints with `m: true` |
| `ws_trade_snapshot.json` | the `e:"tradeSnapshot"` backfill sent on subscribe |
| `ws_ticker.json` | `BTCUSDT@ticker` |

## Spot

Captured live on 2026-10-06 from the spot hosts, no credentials. Spot is a
separate exchange: REST on `api-spot.weex.com`, the socket on
`wss://ws-spot.weex.com/v3/ws/public` with the contract socket's protocol.

| file | source |
| --- | --- |
| `spot_rest_exchange_info.json` | `GET /api/v3/exchangeInfo`, trimmed to BTCUSDT, ETHBTC, MORPHOUSDT and the halted MEMEFIUSDT |
| `spot_rest_ticker_24h.json` | `GET /api/v3/market/ticker/24hr` (every symbol, weight 2), trimmed to three rows |
| `spot_rest_time.json` | `GET /api/v3/time` |
| `spot_ws_book_ticker.json` | first `BTCUSDT@bookTicker` push |
| `spot_ws_trade.json` | first live `BTCUSDT@trade` push after the backfill |
| `spot_ws_ticker.json` | first `BTCUSDT@ticker` push |

### Spot private

No WEEX spot credential has been used here. The refusals and the tradable list
are live captures from 2026-10-06, taken without credentials; the rest are the
spot API pages' documented examples, written as one line.

| file | source | edits |
| --- | --- | --- |
| `spot_rest_api_trading_symbols.json` | `GET /api/v3/apiTradingSymbols`, live | trimmed to BTCUSDT, ETHUSDT and SOLUSDT |
| `spot_rest_error_unauthorized.json` | `GET /api/v3/account` with no headers, HTTP 401, live | none |
| `spot_rest_error_bad_body.json` | `POST /api/v3/order` with an empty body, HTTP 400, live | none |
| `spot_rest_error_unknown.json` | `POST /api/v3/order` with `Content-Type: text/plain`, HTTP 500, live | none |
| `spot_rest_order_ack.json` | Place Order (`POST /api/v3/order`) | `clientOrderId` replaced with a ULID |
| `spot_rest_cancel_ack.json` | Cancel Order (`DELETE /api/v3/order`) | none |
| `spot_rest_order.json` | Get Order Details (`GET /api/v3/order`) | `clientOrderId` replaced with a ULID |
| `spot_rest_open_orders.json` | Get Current Open Orders (`GET /api/v3/openOrders`) | row 1 given a ULID; row 2 added: an ETHBTC sell, `PARTIALLY_FILLED`, 0.008 of 0.02 |
| `spot_rest_my_trades.json` | Get Trade History (`GET /api/v3/myTrades`) | row 2 added: a sell of 0.002 at 69000.00 |
| `spot_rest_account.json` | Get Account Information (`GET /api/v3/account`) | none |
| `spot_ws_private_ack.json` | Order Channel acknowledgement | none |
| `spot_ws_private_ping.json` | the private ping from the WebSocket overview | none |
| `spot_ws_private_orders.json` | Order Channel push | none |
| `spot_ws_private_orders_new.json` | the same push | a resting limit: `type` LIMIT, `timeInForce` GTC, `status` NEW, `price` 105008.00, `size` 0.000952, a ULID `clientOrderId`, fill fields zeroed |
| `spot_ws_private_fill.json` | Fill Channel push | none |
| `spot_ws_private_account.json` | Account Channel push | none |

## Private

Captured on 2026-09-13 with no auth headers:

| file | source |
| --- | --- |
| `rest_error_unauthorized.json` | `GET /capi/v3/account/balance`, HTTP 401 |
| `rest_error_bad_body.json` | `POST /capi/v3/order` with an empty body, HTTP 400 |

WEEX's documented response examples, copied from each endpoint page and written
as one line. To regenerate one, copy the page example and reapply its edits.

| file | page | edits |
| --- | --- | --- |
| `rest_order_ack.json` | Place Order (`POST /capi/v3/order`) | `clientOrderId` replaced with a 26-char ULID |
| `rest_cancel_ack.json` | Cancel Order (`DELETE /capi/v3/order`) | `origClientOrderId` replaced with a ULID |
| `rest_cancel_batch.json` | Cancel Orders Batch (`DELETE /capi/v3/batchOrders`) | the two client ids replaced with ULIDs |
| `rest_open_orders.json` | Get Current Orders (`GET /capi/v3/openOrders`) | row 1 given a ULID; row 2 added: `executedQty` 0.008 of `origQty` 0.02, `status` `"NEW"` |
| `rest_user_trades.json` | Get Trade Details (`GET /capi/v3/userTrades`) | none |
| `rest_positions.json` | Get All Positions (`GET /capi/v3/account/position/allPosition`) | rows 2 and 3 added: an opposite-side BTCUSDT row and a DOGEUSDT row |
| `rest_balance.json` | Get Account Balance (`GET /capi/v3/account/balance`) | none |
| `rest_income.json` | Get Account Income (`POST /capi/v3/account/income`) | rows 2 and 3 added: a settlement with negative `income` and a `transfer_in` |
