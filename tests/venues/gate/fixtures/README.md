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
| `ws_order_book_straddle.json`, `ws_order_book_straddle_next.json` | live — see "the seed triple" |

The two `order_book_update` files were chosen as a consecutive pair off one
socket so the update-id chain is a real observation and not an arrangement.

## REST

| File | Provenance |
| --- | --- |
| `rest_server_time.json` | live — `GET /api/v4/spot/time` |
| `rest_order_book.json` | live — `GET /api/v4/futures/usdt/order_book?contract=BTC_USDT&limit=5`, **without** `with_id` |
| `rest_order_book_seed.json` | live — the same call with `limit=100&with_id=true`, recaptured 2026-08-17; see "the seed triple" |
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

## The seed triple

`rest_order_book_seed.json`, `ws_order_book_straddle.json` and
`ws_order_book_straddle_next.json` are one capture, not three files that happen
to be about depth: the socket was recorded first, the REST book was read three
seconds in, and the two frames kept are the one whose `[U, u]` contains that
snapshot's `id + 1` and the one immediately after it. A seed and a stream taken
minutes apart replay into a *crossed* book, which is what the book tests exist
to catch, so provenance here is the test.

`rest_order_book.json` is kept alongside it precisely because it lacks `id`:
the default call returns levels with no place in the update chain, and the
parser has to refuse that body rather than seed a book at id zero.

Three facts the capture pins down, all measured rather than read off the docs:

- The chain is exactly contiguous: `U == the previous frame's u + 1` held on
  **83 of 83** consecutive frames, no id repeated and none skipped. There is no
  `pu` field and none is needed.
- The frame that continues a seed is the one containing `id + 1`, not `id` —
  `id` is the last update already folded into those levels.
- **Gate maintains the window itself.** Subscribed at 100 levels and seeded at
  100, the book stayed at exactly 100 a side across every applied frame: what
  falls out of the window arrives as an `s: 0` delete. Nothing has to be pruned
  and nothing erodes, which is the load-bearing difference from binance's
  full-depth stream.
- **20, 50 and 100 are the whole tier list.** `200` and `400` are refused with
  `{"code":2,"message":"provided level not supported: N"}`, so 100 is both the
  deepest tier Gate offers and the most `k_book_levels` can hold. Re-measured
  on the 2026-08-17 capture: 483 consecutive frames, 0 gaps, 49 of them
  carrying no levels at all.
- About a third of frames (27 of 84 in one sample) carry **no levels at all**.
  They still advance the chain, so a reader that skips them breaks every later
  contiguity check.

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
