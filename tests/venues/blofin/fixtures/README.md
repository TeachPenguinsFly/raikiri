# blofin fixtures

Captured live on 2026-09-25 between 05:48 and 05:50 UTC from
`openapi.blofin.com`, no credentials. Socket frames come from one
`wss://openapi.blofin.com/ws/public` session and are verbatim. REST bodies are
trimmed: whole rows were cut out of the original text without re-serialising,
so every number keeps the venue's spelling.

| file | source |
|---|---|
| `rest_instruments.json` | `GET /api/v1/market/instruments`, trimmed to BTC-USDT, BTC-USDC, BTC-USD (inverse), 1000BONK-USDT, NVDA-USDT (stock), WTIOIL-USDT and ONE-USDT |
| `rest_funding_rate.json` | `GET /api/v1/market/funding-rate`, trimmed to six rows: 8 h, 4 h and the two 1 h contracts |
| `rest_open_interest.json` | `GET /api/v1/market/open-interest` on 2026-10-06, five of 505 rows kept verbatim, BTC-USDT first |
| `rest_mark_price.json` | `GET /api/v1/market/mark-price`, trimmed to BTC-USDT and BTC-USD |
| `ws_books_snapshot.json` | `books` WTIOIL-USDT, the snapshot (400 levels a side, `prevSeqId` "0") |
| `ws_books_update.json`, `ws_books_update_next.json` | the two deltas that followed it |
| `ws_trades.json` | `trades` BTC-USDT, one print |
| `ws_tickers.json`, `ws_tickers_inverse.json` | `tickers` BTC-USDT and BTC-USD |
| `ws_funding_rate.json` | `funding-rate` BTC-USDT |
| `ws_subscribe_ack.json` | the acknowledgement of one argument |
| `ws_error_invalid_request.json` | the refusal of a frame naming an unlisted instrument (code 60012) |

`ws_books_update_wide.json` was captured on 2026-10-03 at 15:12 UTC from
`wss://openapi.blofin.com/ws/public`, `books` for every live perp over three
sockets: the widest diff in eight minutes and 467 000 book frames, CT-USDT with
482 bids and 332 asks. Verbatim.

Order entry fixtures, from `docs.blofin.com`, read 2026-10-11 (the docs print 18-decimal strings):

| file | source |
|---|---|
| `rest_place_order.json`, `rest_cancel_order.json` | `POST /api/v1/trade/order`, `POST /api/v1/trade/cancel-order` |
| `rest_orders_pending.json` | `GET /api/v1/trade/orders-pending` |
| `rest_fills_history.json` | `GET /api/v1/trade/fills-history` |
| `rest_positions.json`, `rest_balance.json` | `GET /api/v1/account/positions`, `GET /api/v1/account/balance` |
| `ws_orders_snapshot.json` | the private `orders` channel |
| `ws_login.json`, `ws_login_failed.json` | the private socket's login answers |

Live, 2026-10-11, no account: `rest_error_no_such_key.json` is `GET /api/v1/account/balance` with
made-up headers (HTTP 200).
