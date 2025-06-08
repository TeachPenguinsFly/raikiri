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

## Order entry

Added 2026-08 with the execution client. Gate's private endpoints need
credentials this work had none of, so the split below is the important part of
this section: three files are live captures, the rest are **assembled**.

### Live

| File | Provenance |
| --- | --- |
| `rest_currency_pairs.json` | live — `GET /api/v4/spot/currency_pairs`, trimmed from 2230 rows to four, unedited |
| `rest_error_invalid_key.json` | live — `POST /api/v4/futures/usdt/orders` with a fabricated `KEY`/`SIGN` |
| `rest_error_request_expired.json` | live — the same call with a `Timestamp` an hour stale |

`rest_currency_pairs.json` keeps four rows on purpose: BTC_USDT (`precision` 1,
`amount_precision` 6), ETH_USDT (2 and 4), ALEPH_USDT (`amount_precision` **0**,
which is the whole-units encoding the mapper has to survive) and XAR_USDT
(`trade_status: untradable` — a row the loader must skip).

The two error bodies are what pinned the header contract down, and both are
worth not rediscovering:

- **The `Timestamp` header is SECONDS.** A millisecond value is answered
  `REQUEST_EXPIRED`, not accepted. The window is **60 s** either way, and the
  message quotes both clocks.
- **Header names are `KEY`, `SIGN`, `Timestamp`**, and Gate validates them in
  that order — a request missing all three complains about `Timestamp` first.
- `REQUEST_EXPIRED` is our clock, not our key. Filing it under `AuthFailure`
  (where Gate's own docs and ccxt put it) halts a venue whose keys are fine.

### Assembled

No credentials, so nothing below was observed on a socket. Field names, types
and enumerations come from Gate's own published SDK models — `gateapi-python`'s
`FuturesOrder` / `Order` / `Position` / `MyFuturesTrade` / `Trade`, and
`gatews`'s `response_future.go`, which is the websocket shape rather than the
REST one. Values are internally consistent with `rest_contracts.json`
(BTC_USDT: 0.1 tick, one contract a lot) and `rest_currency_pairs.json`
(BTC_USDT spot: 0.1 tick, 0.000001 lot).

| File | Shape it pins |
| --- | --- |
| `ws_futures_order_new.json` | `futures.orders`, `finish_as: "_new"` — the acceptance |
| `ws_futures_order_partial.json` | `finish_as: "_update"`, `left` 5 of `size` 10 |
| `ws_futures_order_cancelled.json` | `finish_as: "cancelled"`, `status: "finished"` |
| `ws_futures_order_poc_rejected.json` | `finish_as: "poc"` — an expiry, not a cancel |
| `ws_futures_order_short.json` | `size` and `left` both **negative** |
| `ws_futures_usertrade.json` | `futures.usertrades` — and no `left` anywhere on it |
| `ws_spot_order_put.json`, `ws_spot_order_finished.json`, `ws_spot_order_cancelled.json` | `spot.orders` with its per-order `event`: `put` / `finish` |
| `ws_spot_usertrade.json` | `spot.usertrades`, millisecond stamps as **strings** |
| `rest_futures_order.json` | the body a successful `POST /futures/usdt/orders` returns |
| `rest_futures_open_orders.json` | `status=open` list; the second row's `text` is `api`, i.e. not ours |
| `rest_spot_open_orders.json` | `GET /spot/open_orders` — grouped **per currency pair** |
| `rest_futures_positions.json` | a short position: `size: -30` |
| `rest_futures_my_trades.json` | `create_time` as **fractional seconds** |
| `rest_spot_my_trades.json` | the same pull on spot, with `create_time_ms` as a string |
| `rest_futures_account.json` | `user`, which the futures private socket demands |

These prove the parsers read the documented shapes; they do not prove those
shapes are what production sends. Replace them with captures the first time
this adapter runs against a real key.

### Field notes worth not rediscovering

- **`_new` and `_update` are websocket-only `finish_as` values.** They mean
  "created" and "filled, partially filled, or amended" — not finish reasons.
  Reading them as terminal closes an order that is still working. No REST body
  ever carries them.
- **A user trade never states the remaining size**, on either API. `left` lives
  on the orders channel only, so the two frames have to be joined by venue
  order id before a fill can state the leaves the OMS treats as authoritative.
- **Futures carries the side in the SIGN of `size` and `left`**; spot has a
  `side` field and unsigned decimals. The same struct has to hold both.
- **Gate's `text` is not only ours.** `api`, `web`, `app`, `liquidation` and
  `liq-...` are reserved source labels on orders nobody here placed, which is
  why only a `t-` prefix counts as a client order id.
- **`t-` plus a 26-char ULID is 28 bytes**, and Gate's limit is 28 bytes
  *after* the prefix. It fits with room to spare, and every Crockford base32
  character is inside Gate's allowed set.
- **The spot open-order body wraps orders in objects that also carry
  `currency_pair`.** A reader that does not require `id` reads the wrappers as
  orders and reports a list of empty ones instead of failing.
