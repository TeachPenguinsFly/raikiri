# Gate.io fixtures

Captured on 2026-08-10 and 2026-08-11 from production (`api.gateio.ws`,
`wss://fx-ws.gateio.ws/v4/ws/usdt`) without credentials, unless a row says
otherwise. Every file is one compact line. Websocket frames are verbatim. The
futures socket keepalive is `{"time":t,"channel":"futures.ping"}`.

## Websocket

| file | source |
| --- | --- |
| `ws_subscribe_ack.json` | the `futures.book_ticker` subscribe answer |
| `ws_error.json` | the subscribe answer for `NOPE_USDT` |
| `ws_pong.json` | the answer to `{"time":t,"channel":"futures.ping"}` |
| `ws_book_ticker.json` | `futures.book_ticker` on BTC_USDT |
| `ws_trades.json` | `futures.trades` on BTC_USDT, a taker sell |
| `ws_trades_buy.json` | `futures.trades` on BTC_USDT, a taker buy |
| `ws_tickers.json` | `futures.tickers` on BTC_USDT |
| `ws_order_book_update.json`, `ws_order_book_update_next.json` | two consecutive `futures.order_book_update` frames off one socket |
| `ws_order_book_straddle.json`, `ws_order_book_straddle_next.json` | see "the seed triple" |
| `ws_spot_order_book.json` | `spot.order_book` on BTC_USDT at 20 levels; frame stamp 2026-08-27 |
| `ws_spot_public_trades.json` | `spot.trades` on BTC_USDT; frame stamp 2026-08-27 |

## REST

| file | source |
| --- | --- |
| `rest_server_time.json` | `GET /api/v4/spot/time` |
| `rest_order_book.json` | `GET /api/v4/futures/usdt/order_book?contract=BTC_USDT&limit=5`, without `with_id` |
| `rest_order_book_seed.json` | the same call with `limit=100&with_id=true`, recaptured 2026-08-17; see "the seed triple" |
| `rest_error_contract_not_found.json` | HTTP 400 from `contract=NOPE_USDT` |
| `rest_error_invalid_param.json` | assembled: label and message copied from a live HTTP 400 (`limit=abc`) |
| `rest_contracts.json` | `GET /api/v4/futures/usdt/contracts`, trimmed to four rows, one edited (below) |
| `rest_futures_tickers.json` | `GET /api/v4/futures/usdt/tickers`, re-serialized with spaces; no capture date recorded |

`rest_contracts.json` keeps `BTC_USDT`, `ETH_USDT` and `SATS_USDT` verbatim.
`DOGE_USDT` is edited: `in_delisting` set to `true` and `status` to
`"delisting"`, because no live row was delisting at capture time.

## The seed triple

`rest_order_book_seed.json`, `ws_order_book_straddle.json` and
`ws_order_book_straddle_next.json` come from one capture. Record the socket at
100 levels, read the REST book (`with_id=true`) a few seconds in, then keep the
frame whose `[U, u]` contains the snapshot's `id + 1` and the frame after it. A
seed and a stream taken minutes apart replay into a crossed book.

## Order entry, added 2026-08

### Captured unauthenticated

| file | source |
| --- | --- |
| `rest_currency_pairs.json` | `GET /api/v4/spot/currency_pairs`, trimmed to BTC_USDT, ETH_USDT, ALEPH_USDT and XAR_USDT, unedited |
| `rest_spot_tickers.json` | `GET /api/v4/spot/tickers` on 2026-10-06, trimmed from 2203 rows to BTC_USDT, ETH_USDT, SOL_USDT, DOGE_USDT and one other |
| `rest_error_invalid_key.json` | `POST /api/v4/futures/usdt/orders` with a fabricated `KEY`/`SIGN` |
| `rest_error_request_expired.json` | the same call with a `Timestamp` an hour stale (the header is in seconds) |

### Assembled

No credentials were available, so these follow Gate's SDK models:
`gateapi-python`'s `FuturesOrder`, `Order`, `Position`, `MyFuturesTrade` and
`Trade`, and `gatews`'s `response_future.go` for websocket shapes. Values match
`rest_contracts.json` and `rest_currency_pairs.json`. Replace them with captures
the first time the adapter runs against a real key.

| file | shape |
| --- | --- |
| `ws_futures_order_new.json` | `futures.orders`, `finish_as: "_new"` |
| `ws_futures_order_partial.json` | `finish_as: "_update"`, `left` 5 of `size` 10 |
| `ws_futures_order_cancelled.json` | `finish_as: "cancelled"`, `status: "finished"` |
| `ws_futures_order_poc_rejected.json` | `finish_as: "poc"` |
| `ws_futures_order_short.json` | `size` and `left` both negative |
| `ws_futures_usertrade.json` | `futures.usertrades` |
| `ws_spot_order_put.json`, `ws_spot_order_finished.json`, `ws_spot_order_cancelled.json` | `spot.orders` with `event` `put` / `finish` |
| `ws_spot_usertrade.json` | `spot.usertrades` |
| `rest_futures_order.json` | a successful `POST /futures/usdt/orders` body |
| `rest_futures_open_orders.json` | `status=open` list; the second row's `text` is `api` |
| `rest_spot_open_orders.json` | `GET /spot/open_orders` |
| `rest_futures_positions.json` | a short position, `size: -30` |
| `rest_futures_my_trades.json` | futures `my_trades` |
| `rest_spot_my_trades.json` | spot `my_trades` |
| `rest_futures_account.json` | futures account, carrying `user` |

## Websocket API, added 2026-10-03

The Gate docs pages (`https://www.gate.com/docs/developers/futures/ws/en/`
and `https://www.gate.com/docs/developers/apiv4/ws/en/`) could not be read
from here: a plain fetch gets an Akamai 403 and the fetch tool stops before
the Websocket API section. So no file below is a doc example. The envelope
comes from two live replies, and `gatews`'s `model.go` (`ResponseHeader`,
`data.result`, `data.errs`) agrees with it.

### Captured unauthenticated

A read-only `order_status` request with no login, sent on 2026-10-03. No order
was sent. The `client_id` IP is replaced with `203.0.113.7`; everything else
is verbatim.

| file | source |
| --- | --- |
| `ws_api_not_login.json` | `futures.order_status` on `wss://fx-ws.gateio.ws/v4/ws/usdt`, answered `AUTHENTICATION_FAILED` / `Not login` |
| `ws_api_spot_not_login.json` | `spot.order_status` on `wss://api.gateio.ws/ws/v4/`, the same refusal |

### Assembled from that envelope

Each file is the captured header with its `channel` and `status` changed.

| file | how |
| --- | --- |
| `ws_api_login.json` | `futures.login`, status 200, `data.result` holding a zeroed `api_key` and a placeholder `uid` |
| `ws_api_login_failure.json` | `futures.login`, status 400, with the label and message of `rest_error_invalid_key.json` |
| `ws_api_order_place_ack.json` | `futures.order_place` with `ack: true`; `data.result` echoes the request |
| `ws_api_order_place.json` | `futures.order_place` with `ack: false`; `data.result` is `rest_futures_order.json` |
| `ws_api_order_cancel_not_found.json` | `futures.order_cancel`, status 400, label `ORDER_NOT_FOUND` |

## Options, captured live on 2026-10-04

From production with no credentials. The contracts listing is trimmed and
written back without whitespace; the fields are as served. The websocket files
are single frames copied verbatim from sessions on
`wss://op-ws.gateio.live/v4/ws`.

| file | source |
|---|---|
| `rest_option_underlyings.json` | `GET https://api.gateio.ws/api/v4/options/underlyings`, as served |
| `rest_option_contracts.json` | `GET /api/v4/options/contracts?underlying=BTC_USDT` cut to five rows, plus the first row of `underlying=DOGE_USDT` |
| `ws_option_contract_tickers.json` | `options.contract_tickers` with payload `["BTC_USDT-20261225-90000-C"]` |
| `ws_option_book_ticker.json` | `options.book_ticker` with payload `{"underlyings":["BTC_USDT"]}` |
| `ws_option_trades.json` | `options.trades` with payload `{"underlyings":[...]}`, the one print in fifteen minutes |

