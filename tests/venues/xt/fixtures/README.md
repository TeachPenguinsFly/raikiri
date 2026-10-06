# XT fixtures

XT serves futures and spot from separate hosts with different envelopes:

- futures REST `https://fapi.xt.com`, WS `wss://fstream.xt.com/ws/market`;
- spot REST `https://sapi.xt.com`, WS `wss://stream.xt.com/public`.

Public files were captured from production XT, the futures files on
2026-08-10/11 and the spot files on 2026-08-26, unless a row gives another
date. They are verbatim bodies and frames written as one compact line; edited
captures say so. REST was fetched with plain `curl`. Futures WS was read with
`websocat -B 200000` piped through `head -N`, since websocat cannot write
straight to a file. Frames are uncompressed UTF-8 JSON.

## Futures REST

| file | source |
| --- | --- |
| `rest_server_time.json` | `GET /future/market/v1/public/time` |
| `rest_depth.json` | `GET /future/market/v1/public/q/depth?symbol=btc_usdt&level=5` |
| `rest_funding_rate.json` | `GET /future/market/v1/public/q/funding-rate?symbol=btc_usdt` |
| `rest_funding_rate_plain.json` | the same endpoint with `symbol=eth_usdt` |
| `rest_error_invalid_symbol.json` | the funding-rate endpoint with `symbol=nope_usdt` |
| `rest_error_invalid_param.json` | `GET /future/market/v1/public/q/depth?symbol=btc_usdt&level=abc` |
| `rest_symbol_list.json` | `GET /future/market/v1/public/symbol/list`, edited: five rows (`btc_usdt`, `eth_usdt`, `ftt_usdt`, `dia_usdt`, `btc_usdt_261225`) kept untouched and the envelope rebuilt |
| `rest_symbol_list_twlo.json` | the same call on 2026-09-26, sliced verbatim to `twlo_usdt` |
| `rest_futures_bills.json` | signed `GET /future/user/v1/balance/bills` on a live account: four rows (a FUND received, a FUND paid, a FEE, a CLOSE_POSITION) chosen from a 100-row capture, `accountId` replaced with a placeholder |

## Futures WebSocket

| file | source |
| --- | --- |
| `ws_depth.json` | `depth@btc_usdt,50,100ms`, recaptured 2026-08-17 |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `depth_update@btc_usdt`, two consecutive frames (the second's `pu` equals the first's `u`) |
| `ws_trade_buy.json` | `trade@btc_usdt` with `m: "BID"` |
| `ws_trade_sell.json` | `trade@btc_usdt` with `m: "ASK"` |
| `ws_mark_price.json` | `mark_price@btc_usdt` |
| `ws_index_price.json` | `index_price@btc_usdt` on `fstream.xt.com/ws/market`, the first push after the ack, 2026-10-06 |
| `ws_agg_ticker.json` | `agg_ticker@btc_usdt` |
| `ws_ticker.json` | `ticker@btc_usdt` |
| `ws_subscribe_ack.json` | the answer to an accepted subscribe |
| `ws_error_unknown_topic.json` | the refusal for `funding_rate@btc_usdt` |

## Spot

| file | source |
| --- | --- |
| `rest_spot_server_time.json` | `GET /v4/public/time` |
| `rest_spot_symbol_list.json` | `GET /v4/public/symbol`, edited: six rows (`btc_usdt`, `eth_usdt`, `sss_usdt`, `btx_usdt`, `sats_usdt`, `tora_usdt`) sliced out by brace matching without re-serialising, and the envelope rebuilt |
| `rest_spot_error_auth.json` | `GET /v4/open-order` with no headers (`{"rc":1,"mc":"AUTH_001"}` at HTTP 200) |
| `rest_spot_error_not_found.json` | `GET /v4/nope` |
| `ws_spot_depth.json` | `depth@btc_usdt,50` on `stream.xt.com/public`; capture date not recorded |
| `ws_spot_ticker.json` | `ticker@btc_usdt` on `stream.xt.com/public`, the first push after the ack, 2026-10-06 |
| `ws_spot_public_trade.json` | `trade@btc_usdt` on `stream.xt.com/public`; capture date not recorded |

## Private

No XT credential has placed an order here. One file is an unauthenticated
capture:

| file | source |
| --- | --- |
| `rest_futures_error_no_key.json` | `POST fapi.xt.com/future/trade/v1/order/create` with no headers, 2026-08-26 |

The rest are built from the payload examples in `github.com/XtApis/xt-api`, the
docs repo behind doc.xt.com. Field names and types are the doc's. Symbols,
prices and quantities were changed to values the catalogue fixtures above
carry, and `clientOrderId` is `02WP8qFtGJTkHk10j5u4aZ` (the ULID
`01ARZ3NDEKTSV4RRFFQ69G5FAV` in 22 characters). `rest_futures_balance_list.json`
mixes quoted and bare numbers and gives `usdt` a twelve-digit walletBalance;
`rest_spot_open_orders.json` keeps the doc's `clientOrderId` `myOrder123` on its
second row.

| file | reference shape |
| --- | --- |
| `rest_futures_open_orders.json` | `POST /future/trade/v1/order/list-open-order`, one NEW row and one PARTIALLY_FILLED |
| `rest_futures_order_list.json` | `POST /future/trade/v1/order/OrderList`, the paged form (`result.items`) |
| `rest_futures_trade_list.json` | `GET /future/trade/v1/order/trade-list`, one MAKER row and one TAKER row |
| `rest_futures_positions.json` | `GET /future/user/v1/position/list`, one SHORT and one flat |
| `rest_futures_balance_list.json` | `GET /future/user/v1/balance/list`, four rows: quoted, bare-number, no `availableBalance`, and the doc's `"coin": ""` row |
| `rest_futures_order_ack.json` | the create/cancel answer |
| `rest_listen_key.json` | `GET /future/user/v1/user/listen-key` |
| `rest_ws_token.json` | `POST /v4/ws-token`, the doc's sample `accessToken` verbatim |
| `rest_spot_order_ack.json`, `rest_spot_cancel_ack.json` | `POST /v4/order` and `DELETE /v4/order/{id}` |
| `rest_spot_open_orders.json` | `GET /v4/open-order`, one partially filled row and one placed elsewhere |
| `rest_spot_trades.json` | `GET /v4/trade` |
| `ws_futures_order_new.json`, `ws_futures_order_partially_canceled.json` | the `order@{listenKey}` push |
| `ws_futures_trade.json` | the `trade@{listenKey}` push |
| `ws_spot_order_new.json`, `ws_spot_order_partial.json` | the spot `order` push |
| `ws_spot_trade.json` | the spot `trade` push |
| `ws_private_ack.json`, `ws_private_listen_key_invalid.json` | the subscribe answer at `code` 0 and at `code` 2 |

The signature vectors in `venue_test.cpp` are not fixtures. They were computed
from the construction in XT's Go SDK (`xt-com/xt-go-demo`,
`xt/v4/{spot,future}/utils.go`), because the example signature on XT's doc page
does not reproduce.
