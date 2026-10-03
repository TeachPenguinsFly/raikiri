# okx fixtures

The websocket and REST files were captured live from production OKX on
2026-08-10 unless a row says otherwise, from public endpoints with no
credentials. The private files are OKX's documented examples, since order entry
needs credentials this repository does not have.

## Websocket, `wss://ws.okx.com:8443/ws/v5/public`

One session subscribing `bbo-tbt`, `trades`, `funding-rate` and `books` on
`BTC-USDT-SWAP`, read for 15 s.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `bbo-tbt` acknowledgement of that subscribe |
| `ws_bbo_tbt.json` | first `bbo-tbt` push |
| `ws_trades.json` | first `trades` push |
| `ws_funding_rate.json` | the `funding-rate` push that follows a subscribe |
| `ws_books_snapshot.json` | the `books` snapshot, 400 levels a side, untrimmed |
| `ws_books_update.json`, `ws_books_update_next.json` | the next two `books` deltas, consecutive frames off the same socket |
| `ws_error.json` | a second session subscribing to a channel that does not exist (code 60018) |
| `ws_tickers.json`, `ws_tickers_spot.json` | `tickers` on BTC-USDT-SWAP and BTC-USDT, 2026-09-04 (from the frames' `ts`) |
| `ws_tickers_pepe.json` | `tickers` on PEPE-USDT-SWAP, 2026-09-26 (from the frame's `ts`) |

## REST, `https://www.okx.com`

| file | source |
|---|---|
| `rest_instruments.json` | `GET /api/v5/public/instruments?instType=SWAP` plus one row from `?instType=FUTURES`; four verbatim rows (BTC-USDT-SWAP, ETH-USDT-SWAP, JP225-USDT-SWAP in `preopen`, BTC-USD-260814) inside the envelope the venue sent |
| `rest_mark_price.json` | `GET /api/v5/public/mark-price?instType=SWAP&instId=BTC-USDT-SWAP` |
| `rest_funding_rate.json` | `GET /api/v5/public/funding-rate?instId=BTC-USDT-SWAP` |
| `rest_server_time.json` | `GET /api/v5/public/time` |
| `rest_error_bad_instrument.json` | the same mark-price call with `instId=NOPE-USDT-SWAP` (code 51001) |

## Private (documented, not captured)

Copied verbatim from the OKX v5 docs (2026-08 revision):

| file | source |
|---|---|
| `ws_orders_fill.json` | the WS Order channel "Push Data Example", a SPOT `BTC-USDT` sell that filled whole |
| `ws_login_ok.json` | the WebSocket Login success response |
| `ws_login_failed.json` | the WebSocket Login failure response, code `60009` |
| `rest_order_ack.json` | `POST /api/v5/trade/order` success response |
| `rest_orders_pending.json` | `GET /api/v5/trade/orders-pending` response |
| `rest_fills.json` | `GET /api/v5/trade/fills` response |
| `rest_positions.json` | `GET /api/v5/account/positions` response (a `MARGIN` row, the one OKX publishes) |
| `rest_account_config.json` | `GET /api/v5/account/config` response |

Assembled from the docs:

| file | how |
|---|---|
| `rest_balance.json` | `GET /api/v5/account/balance`, the published response with a BTC spot row added under `data[0].details`; that row carries `upl` and `availEq` as empty strings. The USDT row is verbatim |
| `rest_order_reject.json` | the documented REST envelope for a refused order (`code "1"`) around the per-order object from the docs' `51008` example, with `clOrdId` set to a ULID |
| `ws_orders_partial_expiry.json` | the `ws_orders_fill.json` object re-valued on `BTC-USDT-SWAP` as a 5-contract IOC that filled 2 and was killed: `state: "canceled"`, `cancelSource: "14"` and a `tradeId` in one message, every number on the 0.1 tick and 0.01 lot |

## Private socket order ops (documented, not captured)

Copied from the OKX v5 docs as published on 2026-10-03, at
`https://www.okx.com/docs-v5/en/#order-book-trading-trade-ws-place-order`,
`#order-book-trading-trade-ws-cancel-order` and `#order-book-trading-trade-ws-amend-order`.
Only the whitespace differs. The docs use the ids `1512` and `1514`, which are
numeric like ours, so the tests use them unchanged. The login replies are
`ws_login_ok.json` and `ws_login_failed.json` above.

| file | source |
|---|---|
| `ws_trade_order.json` | doc example: WS / Place order, successful response |
| `ws_trade_order_failure.json` | doc example: WS / Place order, failure response (`sCode` `5XXXX`, as printed) |
| `ws_trade_format_error.json` | doc example: WS / Place order, the response to a malformed request (code `60013`, empty `data`) |
| `ws_trade_cancel.json` | doc example: WS / Cancel order, successful response |
| `ws_trade_amend_failure.json` | doc example: WS / Amend order, failure response (`sCode` `51008`) |

## Options, captured live on 2026-10-03

From production with no credentials. `rest_option_instruments.json` is the
`data` of two REST answers cut to eight rows and written back without
whitespace; the two `opt-summary` files are single frames cut to their first
rows the same way. Every other file is one frame, verbatim. The websocket
sessions ran on `wss://ws.okx.com:8443/ws/v5/public`: one subscribed
`opt-summary` for `BTC-USD`, and `bbo-tbt`, `mark-price` on
`BTC-USD-261004-84900-C`, and `index-tickers` on `BTC-USD`, for 8 s; one
subscribed `opt-summary` for `BTC-USD_UM` and `bbo-tbt` on a deep `_UM` call
for 40 s; one subscribed `option-trades` (`instType` `OPTION`) for the four
BTC and ETH families until eight prints arrived, about nine minutes.

| file | source |
|---|---|
| `rest_option_underlyings.json` | `GET https://www.okx.com/api/v5/public/underlying?instType=OPTION` |
| `rest_option_instruments.json` | `GET /api/v5/public/instruments?instType=OPTION&uly=BTC-USD`, cut to three `BTC-USD` rows, their three `BTC-USD_UM` twins and `BTC-USD-261004-84900-C`, plus the first row of the same call with `uly=SOL-USD` |
| `ws_opt_summary.json` | an `opt-summary` push for `BTC-USD`, cut to the three rows above |
| `ws_opt_summary_um.json` | an `opt-summary` push for `BTC-USD_UM`, cut to two rows |
| `ws_mark_price_option.json` | the first `mark-price` push for `BTC-USD-261004-84900-C` |
| `ws_index_tickers.json` | the first `index-tickers` push for `BTC-USD` |
| `ws_bbo_tbt_option.json` | the first `bbo-tbt` push for `BTC-USD-261004-84900-C` |
| `ws_bbo_tbt_option_empty.json` | the first `bbo-tbt` push for `BTC-USD_UM-261030-59000-C`, both sides empty |
| `ws_option_trades.json` | the first `option-trades` push, an `ETH-USD` print |
