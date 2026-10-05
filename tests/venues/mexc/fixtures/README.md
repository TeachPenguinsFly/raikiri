# mexc fixtures

The market-data files were captured from production MEXC futures on 2026-08-10
unless a row gives another date. They are public endpoints and need no
credentials. Trimmed files keep whole rows sliced verbatim out of the capture.
Do not round-trip a regenerated file through a JSON library: re-serializing
turns `0.000085` into `8.5e-05`, which the fixed-point parser refuses.

The futures websocket sends uncompressed JSON text frames. Order entry needs
credentials, so its success bodies are built from MEXC's API reference; the
last section says which.

## Websocket, `wss://contract.mexc.com/edge`

Subscribe with `{"method":"sub.<channel>","param":{"symbol":"BTC_USDT"}}`, one
symbol per message. The server closes a socket after 60 s without a client
message, so send `{"method":"ping"}` while capturing.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `sub.ticker` acknowledgement |
| `ws_ticker.json` | first `push.ticker` after it |
| `ws_ticker_inverse.json` | first `push.ticker` for `BTC_USD`, 2026-09-26 |
| `ws_deal.json` | a `push.deal` carrying four prints |
| `ws_depth_full.json` | first `push.depth.full` of a `limit:50` subscribe, recaptured 2026-08-17 |
| `ws_depth_full_nex.json`, `ws_depth_full_floki.json`, `ws_depth_full_zbt.json` | `push.depth.full` on `NEX_USDT`, `FLOKI_USDT` and `ZBT_USDT`; frame stamps are 2026-09-26 |
| `ws_depth_update.json`, `ws_depth_update_next.json` | the first two `push.depth` deltas, consecutive frames off one socket |
| `ws_pong.json` | the answer to `{"method":"ping"}` |
| `ws_error.json` | `sub.ticker` on `NOSUCH_USDT` |
| `ws_idle_timeout.json` | the close frame after a socket was held 100 s sending nothing; it closed at 62 s |

## Spot websocket, `wbs-api.mexc.com`

Protobuf frames, kept as raw `.pb` bytes. No capture date was recorded.

| file | channel |
|---|---|
| `ws_spot_book_ticker.pb` | `spot@public.aggre.bookTicker.v3.api.pb@100ms@BTCUSDT` |
| `ws_spot_deals.pb` | `spot@public.aggre.deals.v3.api.pb@100ms@BTCUSDT` |
| `ws_spot_depth.pb` | `spot@public.limit.depth.v3.api.pb@BTCUSDT@20` |

## REST, `https://contract.mexc.com`

| file | source |
|---|---|
| `rest_contract_detail.json` | `GET /api/v1/contract/detail`, trimmed to `BTC_USDT`, `ETH_USDT`, `AIINU_USDT` and `SATS_USDT` |
| `rest_contract_detail_live.json` | the same call, trimmed to 21 rows; no capture date was recorded |
| `rest_contract_detail_inverse.json` | the same call on 2026-09-26, trimmed to the `BTC_USD` row |
| `rest_contract_detail_tradfi.json` | the same call on 2026-10-05, trimmed to stock, metal, oil and gold-token rows and `STOCKER_USDT`, a coin |
| `rest_server_time.json` | `GET /api/v1/contract/ping` |
| `rest_depth.json` | `GET /api/v1/contract/depth/BTC_USDT?limit=5` |
| `rest_ticker.json` | `GET /api/v1/contract/ticker?symbol=BTC_USDT` |
| `rest_funding_rate.json` | `GET /api/v1/contract/funding_rate/BTC_USDT` |
| `rest_funding_rates.json` | `GET /api/v1/contract/funding_rate` with no symbol; pretty-printed and re-serialized, no capture date recorded |
| `rest_error_bad_contract.json` | the ticker call with `symbol=NOSUCH_USDT`, code `1001` |
| `rest_error_bad_parameter.json` | the depth call with `limit=abc`, code `600` |

## Order entry, added 2026-08-26

### Captured unauthenticated

Refusals from production, taken with no key or a made-up one.

| file | source |
|---|---|
| `rest_exchange_info.json` | `GET https://api.mexc.com/api/v3/exchangeInfo`, trimmed to `BTCUSDT`, `ETHUSDT`, `WNTUSDT`, `XENUSDT` and `PYTHUSDT` |
| `rest_spot_ticker_24h.json` | `GET https://api.mexc.com/api/v3/ticker/24hr` on 2026-09-25, trimmed to three rows |
| `rest_error_not_logged_in.json` | `GET contract.mexc.com/api/v1/private/order/list/open_orders/BTC_USDT` with no headers |
| `rest_error_key_expired.json` | the same call against `api.mexc.com` with `ApiKey: bogus` |
| `rest_spot_error_key.json` | `POST api.mexc.com/api/v3/order` with `X-MEXC-APIKEY: bogus` |
| `rest_spot_error_not_found.json` | `GET /api/v3/openOrders?symbol=BTCUSDT` with a key header and no timestamp or signature (HTTP 404) |

`contract.mexc.com` rejects a private POST at the CDN with 403, so private
futures calls go to `api.mexc.com`.

### Built from the API reference

Field names and types are MEXC's; symbols and numbers were changed to ones the
catalogue fixtures above carry.

| file | reference shape |
|---|---|
| `ws_personal_order_filled.json` | the `push.personal.order` payload example, verbatim |
| `ws_personal_order_new.json`, `_partial.json`, `_cancelled.json` | the same shape at `state` 2 with `dealVol` 0, at 2 with `dealVol` 1, and at 4 |
| `ws_personal_deal.json` | the `order/deal_details` row shape on `push.personal.order.deal`. The docs show no example on that channel; the name follows the other personal channels |
| `ws_personal_position.json` | the `push.personal.position` payload example, verbatim |
| `ws_login_ack.json` | `rs.login`, the channel the docs name for a successful login |
| `rest_futures_open_orders.json` | `GET /api/v1/private/order/list/open_orders` |
| `rest_futures_order_deals.json` | `GET /api/v1/private/order/list/order_deals` |
| `rest_futures_positions.json` | `GET /api/v1/private/position/open_positions` |
| `rest_funding_records.json` | `GET /api/v1/private/position/funding_records`: the reference's two rows (the second moved to `SUSHI_USDT`) plus a long and a short at a positive rate written here |
| `rest_futures_cancel_partial.json` | the cancel response example, verbatim |
| `rest_spot_order_new.json` | `POST /api/v3/order` |
| `rest_spot_open_orders.json` | `GET /api/v3/openOrders` |
| `rest_spot_my_trades.json` | `GET /api/v3/myTrades` |
