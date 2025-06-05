# BitMEX fixtures

This directory holds two kinds of file. Everything in the **REST** and
**WebSocket** sections below is a verbatim capture from BitMEX production on
**2026-08-10** (UTC) — except the `XBTEUR` row noted below, captured
**2026-08-11** — trimmed but never reformatted: numbers keep the venue's own
spelling, because that spelling is what the parsers are asserted against. REST writes
`"tickSize":0.000000001`, never `1e-09`; the websocket writes the same kind of
value as `1.0E-4`. Both spellings are in here on purpose, and a re-serialised
fixture would have tested a format the venue does not send.

REST base `https://www.bitmex.com/api/v1`, websocket `wss://ws.bitmex.com/realtime`.
Nothing in those two sections is synthetic and nothing is authenticated.

The last section, **Authenticated surfaces**, is documentation-derived rather
than captured. Read its heading before treating anything under it as evidence
about the live wire.

## REST

| file | provenance |
| --- | --- |
| `rest_instruments.json` | `GET /instrument/active` (HTTP 200, 66 rows, ~110 KiB), trimmed to six rows chosen to cover every branch of `map_instrument`. Row order and bytes are otherwise untouched. |
| `rest_error_validation.json` | `GET /trade?symbol=XBTUSD&count=99999` → **HTTP 400**, `name: ValidationError`. |
| `rest_error_not_found.json` | `GET /nosuchendpoint` → **HTTP 404**, `name: HTTPError`. |
| `rest_error_forbidden.json` | `GET /position` unauthenticated → **HTTP 403**, `name: HTTPError`. Same `name` as the 404, which is why classification leads on the status. |

The seven instrument rows and what each one pins down:

| symbol | `typ` | shape | expected |
| --- | --- | --- | --- |
| `XBTUSD` | FFWCSX | inverse perpetual, `multiplier` -100000000 | Perp, `contract_value` = 1 **USD** |
| `XBTUSDT` | FFWCSX | linear USDT perpetual, `multiplier` 1 | Perp, `contract_value` = 1e-6 **XBT** |
| `XBTZ26` | FFCCSX | dated inverse future, `expiry` 2026-12-25, no funding | Future; **refused** when Perp is asked for |
| `SOL_USDT` | IFXXXP | spot, `underlyingToPositionMultiplier` 1e9 | Spot, `contract_value` = 1e-9 SOL |
| `ETHUSD` | FFWCSX | **quanto** (`isQuanto: true`) | **refused** — no single-currency contract size exists |
| `XRPU26` | FFCCSX | `state: "Settled"` | **refused** — `/instrument/active` returns it anyway |
| `XBTEUR` | FFWCSX | inverse perpetual **with an `expiry`** (2026-08-11) and funding | Perp, `contract_value` = 1 **EUR** |

The `XRPU26` row is the point of the state check: 16 of the 66 rows the "active"
endpoint returned were `Unlisted` or `Settled`, and a settled contract still
carries a complete, plausible set of tick/lot/multiplier columns.

The `XBTEUR` row is why this file has a second capture date. Between the two
pulls, twelve perpetuals — `XBTEUR`, `DOGEUSDT`, `LTCUSDT`, `XAGUSDT`,
`QQQUSDT`, `PEPEUSDT` and others — grew an `expiry` set to the same noon, while
staying `Open` and still funding: BitMEX schedules a **delisting** by putting an
expiry on a perpetual. Anything that reads `expiry` as "this is a dated future"
drops all twelve the day before they stop trading (it dropped this adapter's
catalogue from 22 instruments to 10). Funding, not expiry, is what separates the
two contract types, and `XBTEUR` is the row that holds that rule in place.

## WebSocket

Captured with
`{ echo '<subscribe>'; sleep N; } | websocat -B 400000 wss://ws.bitmex.com/realtime | head -N`,
subscribing to `quote:XBTUSD`, `trade:XBTUSD`, `instrument:XBTUSD`,
`orderBookL2_25:XBTUSD` (and `quote:NOSUCHSYMBOL` for the refusal).

| file | provenance |
| --- | --- |
| `ws_welcome.json` | Connection banner. No `table`, no `success` — it must not read as data. |
| `ws_subscribe_ack.json` | One ack **per arg**, not per subscribe message. |
| `ws_error.json` | Refusal of `quote:NOSUCHSYMBOL`: `{"status":400,"error":...}`. The only failure shape the socket has. |
| `ws_quote_partial.json` | Snapshot — **two rows, one per liquidity pool**. Primary carries both sides, Secondary only a bid. |
| `ws_quote_insert.json` | Both sides present. |
| `ws_quote_insert_bid_only.json` | **The partial-field pair for `quote`**: same table, same subscription, `askPrice`/`askSize` simply absent. Note the action is `insert`, not `update` — BitMEX drops unchanged columns on inserts too. |
| `ws_trade_partial.json` | Snapshot, 2 prints. Trades are the one table that is never partial per row. |
| `ws_trade_insert.json` | A live XBTUSD print, same column set as the snapshot. |
| `ws_instrument_partial.json` | Snapshot — the full 78-column instrument row, including `fundingRate`, `indicativeFundingRate`, `fundingTimestamp` and `fundingInterval`. |
| `ws_instrument_update_mark.json` | **The partial-field pair for `instrument`**: 4 columns of 78 (`openValue`, `fairPrice`, `markPrice`, `timestamp`). No funding column, so it must not publish a funding event. |
| `ws_instrument_update_quote.json` | A second update shape — `bidPrice`/`midPrice`/`impact*` only. |
| `ws_book_partial.json` | 50 levels, all tagged `pool: "Aggregated"`. Its top bid (2123700 @ 64054.7) is exactly Primary's 80200 plus Secondary's 2043500 from `ws_quote_partial.json` — this is the evidence that pools sum. |
| `ws_book_update.json` | Two levels resized, keyed by `id`. |
| `ws_book_delete.json` | **Carries no `size` at all** — a delete names a level and says nothing about how big it was. |

No 429 body is captured. The unauthenticated budget is 180/minute
(`x-ratelimit-limit: 180` on every response), and deliberately breaching it to
photograph the refusal risks an IP ban for a fixture that would test nothing:
`classify()` reads the status, and the test passes the status directly.

The bare text `pong` (BitMEX's answer to the bare text `ping`) is not a fixture
either — it is not JSON, and the test asserts that by feeding the literal to
`parse_frame`.

## Authenticated surfaces — **documentation-derived, not captured**

Order entry, the reconciliation pulls and the private `order` / `execution` /
`position` tables all require credentials, and this repository has none.
Capturing them would mean placing real orders with real money on a production
account, so the files below are **assembled from BitMEX's own API reference** —
its `Order`, `Execution` and `Position` model definitions and the examples that
accompany them — rather than photographed off a socket.

They are evidence about *shape*: which wrapper a row arrives in, which columns
an `update` omits, where a per-order verdict hides. They are not evidence that
the live wire matches, and no assertion against them should be read that way.

| file | source |
| --- | --- |
| `rest_order_new.json` | `POST /order` — the single `Order` object it answers |
| `rest_order_rejected.json` | the same object with `ordStatus: "Rejected"` |
| `rest_order_open.json` | `GET /order?filter={"open":true}` — a bare array of three |
| `rest_order_cancel.json` | `DELETE /order` — an array where the SECOND row carries `error` |
| `rest_execution_history.json` | `GET /execution/tradeHistory` — a maker Trade, a Funding row, a taker Trade |
| `rest_position.json` | `GET /position` |
| `ws_order_partial.json` | the private `order` table's snapshot, `data`-wrapped |
| `ws_order_update_canceled.json` | an `update` carrying an id, an ordStatus and almost nothing else |
| `ws_execution_insert.json` | a private `execution` insert |
| `ws_auth_failure.json` | `{"status":401,"error":"Signature not valid."}` |

Four of these exist for one reason each, and each reason is a bug that would
otherwise be invisible:

- **`rest_order_cancel.json` puts the failure in the ROW.** BitMEX answers a
  bulk cancel with HTTP 200 and marks the orders it could not cancel by adding
  an `error` column to them. An adapter that reads the status line reports every
  one of those as cancelled, and the OMS then believes a live order is gone.
- **`ws_order_update_canceled.json` carries no symbol, no side and no size.**
  The `order` table is keyed on `orderID` alone, so an update legitimately
  arrives with three columns. Reading an absent column as zero reports a live
  order as one for nothing at a price of nothing; the adapter merges onto what
  it remembered instead, and drops the frame entirely if it remembers nothing.
- **`rest_execution_history.json` includes a `Funding` row.** It carries a
  `commission`, a timestamp and an account, and no price or quantity — booking
  it as a trade invents a position that was never traded. Only
  `execType: "Trade"` is a fill.
- **The maker row's numbers are chosen to be checkable.** 100 contracts of
  XBTUSD at 64000.5 is 100 USD, which on an **inverse** contract settles as
  100 / 64000.5 = 0.001562488 **BTC**; the 0.00075 commission rate on that is
  1171e-9 BTC. `price * quantity` would have been 6 400 050 of no currency at
  all, which is why the fee is derived through `portfolio::settlement_value`.
  The row's own `execComm` (117 satoshis) is left in and deliberately unread:
  it is denominated in the settlement currency's **minor units** and the row
  never says what that currency's scale is.

Symbols were chosen from `rest_instruments.json` so that every row resolves —
or deliberately does not. `XBTUSD` is the inverse perpetual, `SOL_USDT` the
spot pair (a different market with a different step, which is what the pulls'
market filtering is tested against), and `ETHUSD` the **quanto** row the
catalogue refuses: an order and a position on it appear here precisely so the
mappers can be shown dropping them rather than attaching them to instrument
zero.

The signing test uses no fixture. Its three expected digests are the ones
BitMEX's own authentication documentation publishes for its example secret, so
that test checks this implementation against the venue rather than against
itself.
