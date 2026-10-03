# Deribit fixtures

Captured 2026-08-11 against production `www.deribit.com`, unauthenticated, REST
with `curl` and the socket with `websocat -B 400000` against
`wss://www.deribit.com/ws/api/v2`. Public files are verbatim frames or bodies
reformatted to one line. The only edits are the deletions noted per row; no
number, key or ordering was retyped.

## REST, `GET /api/v2/public/...`

| file | source |
| --- | --- |
| `rest_get_time.json` | `get_time` |
| `rest_get_instruments.json` | trimmed: six whole rows taken from `get_instruments` with `currency=BTC&kind=future`, `&kind=option`, `&kind=future&expired=true` and `currency=any&kind=future`, under the envelope of the `kind=future` call. The rows are `BTC-PERPETUAL`, `BTC-25SEP26`, `BTC-11AUG26-64000-C`, `BTC-11AUG26-64000-P`, `BTC-10AUG26` (delivered) and `ADA_USDC-PERPETUAL` (linear) |
| `rest_error_invalid_kind.json` | `get_instruments?currency=BTC&kind=perpetual` |

## WebSocket, `public/subscribe`

| file | source |
| --- | --- |
| `ws_book_bbo.json` | `book.BTC-PERPETUAL.none.1.100ms` |
| `ws_book_bbo_option.json` | `book.BTC-11AUG26-64000-C.none.1.100ms` |
| `ws_book_snapshot.json` | `book.BTC-PERPETUAL.100ms`, `type: "snapshot"`, trimmed to the first 3 levels a side |
| `ws_book_change.json` | `book.BTC-PERPETUAL.100ms`, `type: "change"`, trimmed the same way (5 bids, all 4 asks); its `prev_change_id` is `ws_book_snapshot.json`'s `change_id` |
| `ws_book_snapshot_full.json` | `book.BTC-PERPETUAL.100ms`, `type: "snapshot"`, captured 2026-08-17, whole |
| `ws_book_change_full.json` | the next frame of that capture |
| `ws_trades.json` | `trades.BTC-PERPETUAL.100ms` |
| `ws_ticker_perpetual.json` | `ticker.BTC-PERPETUAL.100ms` |
| `ws_ticker_future.json` | `ticker.BTC-25SEP26.100ms` |
| `ws_ticker_option.json` | `ticker.BTC-11AUG26-64000-C.100ms` |
| `ws_subscribe_ack.json` | the reply to a 5-channel subscribe |
| `ws_subscribe_ack_empty.json` | the reply to `book.BTC-DOESNOTEXIST.none.1.100ms` |
| `ws_error_raw_unauthorized.json` | the reply to `ticker.BTC-PERPETUAL.raw` on an unauthenticated socket (13778) |
| `ws_heartbeat_test_request.json` | the test request sent after `public/set_heartbeat` with `interval: 10` |

## Private / order entry

No credentials exist for this repository. Except for the two verbatim error
captures, these files are constructed from Deribit's published specifications
(`docs.deribit.com/specifications/deribit_openapi.json` and
`deribit_asyncapi.json`, read 2026-08-26 through the `.md` mirrors under
`docs.deribit.com/api-reference/` and `docs.deribit.com/subscriptions/`), with
values on the tick and lot sizes `rest_get_instruments.json` publishes.

| file | source |
| --- | --- |
| `rest_error_unauthorized.json` | verbatim: `GET /api/v2/private/buy?...` with no credentials, 2026-08-26 (13009) |
| `rest_error_method_not_found.json` | verbatim: `GET /api/v2/public/definitely_not_a_method`, 2026-08-26 (-32601) |
| `rest_private_buy_ack.json` | constructed `private/buy` on `BTC-PERPETUAL`, 200 USD at 63500, resting |
| `rest_private_buy_option_ack.json` | constructed `private/buy` on `BTC-11AUG26-64000-C`, 0.3 at 0.0315 BTC, filled on arrival |
| `rest_open_orders.json` | constructed `get_open_orders_by_currency`: a partially filled perpetual, a resting option and a `BTC-DOESNOTEXIST` row |
| `rest_order_state_ioc_cancelled.json` | constructed: an `immediate_or_cancel` order, `order_state: "cancelled"`, 120 of 300 filled |
| `rest_user_trades.json` | constructed `get_user_trades_by_currency`: one maker fill with a negative fee and one option taker fill |
| `rest_positions.json` | constructed `get_positions`: a short perpetual and a long option |
| `ws_user_changes_partial_fill.json` | constructed `user.changes.future.BTC.raw` with one trade and its order |
| `ws_user_changes_accept.json` | constructed `user.changes.option.BTC.raw` with a resting order and no trades |
| `ws_user_changes_market_price.json` | constructed: an `untriggered` stop whose `price` is the string `"market_price"` |
