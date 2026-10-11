# aster fixtures

Captured live on 2026-09-20 from production Aster USD-margined perpetuals,
REST on `https://fapi.asterdex.com` and the socket on
`wss://fstream.asterdex.com/stream`. Every path is public; no credentials.

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.asterdex.com/stream?streams=btcusdt@bookTicker` |
| `ws_agg_trade.json` | `btcusdt@aggTrade` |
| `ws_mark_price.json` | `btcusdt@markPrice@1s` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two consecutive frames of one session (`u` 554245272553 is the next frame's `pu`) |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed to three symbols: one plain USDT perp, one USD1-quoted, one not yet trading |
| `rest_exchange_info_tradfi.json` | the same call on 2026-10-05, trimmed to a coin, a stock, an ETF and a commodity row |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_funding_info.json` | `GET /fapi/v1/fundingInfo`, trimmed to three rows |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_ticker_24h.json` | `GET /fapi/v1/ticker/24hr?symbol=BTCUSDT` |
| `rest_server_time.json` | `GET /fapi/v1/time` |

## Spot

Captured live on 2026-10-06 with no credentials. Spot is its own API on
`sapi.asterdex.com` and `wss://sstream.asterdex.com/stream`, with the futures
API's shapes.

| file | source |
|---|---|
| `spot_rest_exchange_info.json` | `GET /api/v1/exchangeInfo`, trimmed to BTCUSDT, ASTERUSDT, WLFIUSD1 and FORMUSDT and three assets |
| `spot_rest_ticker_24h.json` | `GET /api/v1/ticker/24hr` (every symbol, weight 40), trimmed to three rows |
| `spot_rest_time.json` | `GET /api/v1/time` |
| `spot_rest_depth_snapshot.json` | `GET /api/v1/depth?symbol=BTCUSDT&limit=5` |
| `spot_ws_book_ticker.json` | first `btcusdt@bookTicker` push |
| `spot_ws_depth_update.json`, `spot_ws_depth_update_next.json` | two consecutive `btcusdt@depth@500ms` pushes |
| `spot_ws_trade.json` | first `formusdt@trade` push |

Order entry fixtures, from the v3 (Pro API) futures docs at
`github.com/asterdex/api-docs`, `V3(Recommended)/EN/aster-finance-futures-api-v3.md`, read 2026-10-11:

| file | source |
|---|---|
| `user_order_trade_update.json` | Event: Order Update |
| `user_listen_key_expired.json` | Event: User Data Stream Expired |
| `rest_listen_key.json` | `POST /fapi/v3/listenKey` |
| `rest_query_order.json`, `rest_open_orders.json` | `GET /fapi/v3/order`, `GET /fapi/v3/openOrders` |
| `rest_user_trades.json` | `GET /fapi/v3/userTrades` |
| `rest_position_risk.json` | `GET /fapi/v3/positionRisk`, one-way mode |
| `rest_account.json` | `GET /fapi/v3/account` |
| `rest_income.json` | `GET /fapi/v3/income` |

Live, 2026-10-11, no account: `rest_error_signature.json` is `POST /fapi/v3/order` with a made-up
signature (HTTP 400), `rest_error_no_nonce.json` the same call without a nonce (HTTP 400).
