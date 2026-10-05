# bitget fixtures

The public files were captured from production Bitget on 2026-08-11 without
credentials, unless a row says otherwise. Files were spliced out of the raw
capture as text, so `0.0001` stays `0.0001`; do not round-trip a regenerated
file through a JSON library. The private files are built by hand, listed in
their own section.

## Websocket, `wss://ws.bitget.com/v2/ws/public`

The keepalive is the four bytes `ping`, answered with the text `pong`; a
trailing newline gets an error frame instead.

One session subscribing `ticker`, `trade` and `books5` on `BTCUSDT`, read for 14 s:

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `ticker` acknowledgement |
| `ws_ticker.json` | one `ticker` push |
| `ws_trade.json` | a `trade` push carrying four prints |

Other sessions:

| file | source |
|---|---|
| `ws_books15.json` | a `books15` push on `BTCUSDT`, recaptured 2026-08-17 |
| `ws_books_update.json` | a `books` delta on `BTCUSDT` containing a `"0"` (delete) level |
| `ws_books_update_one_sided.json` | a `books` delta with `"bids":[]` |
| `ws_error_unknown_instrument.json` | subscribing `instId=NOSUCHCOINUSDT`, `code:30001` |
| `ws_error_unrecognized_request.json` | sending `ping\n` instead of `ping`, `code:30002` |

## REST, `https://api.bitget.com`

| file | source |
|---|---|
| `rest_contracts.json` | `GET /api/v2/mix/market/contracts?productType=USDT-FUTURES`, trimmed to `BTCUSDT`, `ETHUSDT`, `SHIBUSDT` and `PEPEUSDT` |
| `rest_contracts_rwa.json` | the same call on 2026-10-05, trimmed to `BTCUSDT` and four `isRwa` rows: a stock, an ETF, gold and the PAXG token |
| `rest_ticker.json` | `GET /api/v2/mix/market/ticker?symbol=BTCUSDT&productType=USDT-FUTURES` |
| `rest_orderbook.json` | `GET /api/v2/mix/market/orderbook?symbol=BTCUSDT&productType=USDT-FUTURES&limit=5` |
| `rest_current_fund_rate.json` | `GET /api/v2/mix/market/current-fund-rate?symbol=BTCUSDT&productType=USDT-FUTURES` |
| `rest_server_time.json` | `GET /api/v2/public/time` |
| `rest_spot_symbols.json` | `GET /api/v2/spot/public/symbols`, trimmed; `requestTime` is 2026-08-25 |
| `rest_error_unknown_symbol.json` | the ticker call with `symbol=NOSUCHUSDT`, code `40034` |
| `rest_error_missing_product_type.json` | the ticker call without `productType`, code `400172` |

## Built by hand

There are no Bitget credentials here. These follow the shapes in Bitget's API
reference, with symbols from `rest_contracts.json`. Most carry placeholder ids
and stamps (`requestTime` 1700000001000, `orderId` 1234567890123456789).

| file | shape |
|---|---|
| `rest_mix_account_bill.json` | `GET /api/v2/mix/account/bill`: the reference's two rows with the symbol and bill id changed, plus a positive `contract_settle_fee` on `ETHUSDT` and one on `SOLUSDT` written here |
| `rest_order_ack.json` | `POST /api/v2/mix/order/place-order` |
| `rest_batch_cancel_partial.json` | `POST /api/v2/mix/order/batch-cancel-orders`, one success and a `failureList` |
| `rest_mix_orders_pending.json` | `GET /api/v2/mix/order/orders-pending` |
| `rest_mix_fills.json` | `GET /api/v2/mix/order/fills` |
| `rest_mix_positions.json` | `GET /api/v2/mix/position/all-position` |
| `rest_spot_unfilled_orders.json` | `GET /api/v2/spot/trade/unfilled-orders` |
| `rest_spot_fills.json` | `GET /api/v2/spot/trade/fills` |
| `ws_login_ack.json` | the private socket's `login` event |
| `ws_orders_mix_accepted.json`, `ws_orders_mix_partial_fill.json`, `ws_orders_mix_filled.json`, `ws_orders_mix_canceled.json` | `orders` pushes on `USDT-FUTURES` |
| `ws_orders_spot_partial_fill.json` | an `orders` push on spot |

## Websocket trade, `wss://ws.bitget.com/v2/ws/private`

No order was placed to get these. They come from the classic docs as
published on 2026-10-03:
`https://www.bitget.com/docs/classic/websocket/contract/private/Place-Order-Channel`,
`.../contract/private/Cancel-Order-Channel`,
`.../spot/private/Place-Order-Channel` and
`https://www.bitget.com/docs/classic/websocket/intro`. The docs use the id
`xxxxx-xxx-xxx-xxxx-xxxxxx`, so a test swaps in a numeric id.

| file | source |
|---|---|
| `ws_trade_place_order.json` | doc example: the `place-order` reply on `USDT-FUTURES`. The docs title it "Fail Response Example", but it carries code 0 and "Success" |
| `ws_trade_cancel_order.json` | doc example: the `cancel-order` reply on `USDT-FUTURES` |
| `ws_trade_spot_place_order.json` | doc example: the `place-order` reply on `SPOT` |
| `ws_login_failure.json` | doc example: the login failure event from the websocket intro |
| `ws_trade_error.json` | assembled. The docs list `event: error` for a refused trade but show none, so this is the place-order reply with that event and the rest code 40017 |
