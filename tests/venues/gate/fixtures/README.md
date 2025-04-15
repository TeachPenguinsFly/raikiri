# Gate.io fixtures

Captured 2026-08-10/11 against **production** (`api.gateio.ws`,
`wss://fx-ws.gateio.ws/v4/ws/usdt`). Every file is one compact line so a
fixture diff shows what changed rather than how it was reformatted.

All websocket frames are verbatim, unedited frames off a live socket. The one
file that is not is `rest_contracts.json`, and that is called out below.

## Websocket

| File | Provenance |
| --- | --- |
| `ws_subscribe_ack.json` | live — `futures.book_ticker` subscribe answer, `result.status == "success"` |
| `ws_error.json` | live — same shape with an `error` object added, from subscribing to `NOPE_USDT` |
| `ws_pong.json` | live — answer to `{"time":t,"channel":"futures.ping"}`; note `event` is `""` and `result` is `null` |
| `ws_book_ticker.json` | live — BTC_USDT top of book |
| `ws_trades.json` | live — BTC_USDT, `size` **negative** (taker sold) |
| `ws_trades_buy.json` | live — BTC_USDT, `size` **positive** (taker bought) |
| `ws_tickers.json` | live — BTC_USDT, carries `funding_rate` **and** `mark_price` |
| `ws_order_book_update.json` | live — BTC_USDT depth delta |
| `ws_order_book_update_next.json` | live — the frame immediately after it; `U == previous u + 1`, and it contains `s: 0` deletes |

The two `order_book_update` files were chosen as a consecutive pair off one
socket so the update-id chain is a real observation and not an arrangement.

## REST

| File | Provenance |
| --- | --- |
| `rest_server_time.json` | live — `GET /api/v4/spot/time` |
| `rest_order_book.json` | live — `GET /api/v4/futures/usdt/order_book?contract=BTC_USDT&limit=5` |
| `rest_error_contract_not_found.json` | live — HTTP 400 from `contract=NOPE_USDT`; ships the label **with no `message` field** |
| `rest_error_invalid_param.json` | **assembled** — the label and message text are verbatim from a live HTTP 400 (`limit=abc`), re-typed here as a standalone file |
| `rest_contracts.json` | **trimmed, and one row edited** — see below |

### `rest_contracts.json`

Four rows taken verbatim from the live 899-row
`GET /api/v4/futures/usdt/contracts` response, except the last:

- `BTC_USDT` — verbatim. tick `0.1`, `quanto_multiplier` `0.0001`, 8 h funding.
- `ETH_USDT` — verbatim. `enable_decimal: true` and `order_size_min: 0`, which
  is the encoding the mapper has to survive.
- `SATS_USDT` — verbatim. `order_price_round` is `0.00000000001`, below what
  `FixedE9` represents, so the mapper must refuse it.
- `DOGE_USDT` — **edited**: `in_delisting` flipped to `true` and `status` to
  `"delisting"`. Everything else is the live row.

The edit is necessary and is the only one in this directory: **zero of the 899
live rows had `in_delisting: true`** and all 899 had `status: "trading"`, so
there was no real row to trim down to. The two edited fields are exactly the
two the mapper refuses on, and both values come from Gate's own documented
enumeration (`prelaunch, trading, delisting, delisted, circuit_breaker`).

## Field notes worth not rediscovering

- **Sizes are bare JSON integers, prices are quoted strings**, in the same
  object (`"b":"64155.8","B":76800`). Gate's SDK documents `order_size_min` as
  a string; the live endpoint sends `1`.
- **`t` is milliseconds. `funding_next_apply` and `create_time` are seconds.**
  Both units appear on the same object in `ws_tickers.json`.
- **`create_time_ms` means different things on different transports.** The
  websocket sends integer milliseconds (`1786395787750`). The REST
  `/futures/usdt/trades` endpoint sends *fractional seconds* under the same
  name (`1786395787.750`). Only the websocket form is parsed by this adapter.
- **The websocket error code is a number** (`{"code":2,...}`) while a REST
  refusal carries a **string label**. They do not share a vocabulary.
- **A successful REST body has no envelope**: the catalogue is a top-level
  array, so the HTTP status is the only success signal.
