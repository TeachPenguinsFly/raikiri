# kraken futures fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

This directory holds two kinds of file and they are not interchangeable.
Everything named in the two sections below was **captured live** from
production Kraken Futures on 2026-08-10 over public endpoints and the public
websocket, with no credentials involved. Frames were copied out of the capture
**verbatim**, never re-serialized: Kraken spells the same field
`0.00001477885` on one contract and `8.907125e-6` on another, and a round trip
through a JSON library would have normalised exactly the difference these tests
exist to pin.

The last section, **Authenticated surfaces**, is documentation-derived rather
than captured, and says so at length. Read that heading before treating
anything under it as evidence about the live wire.

Kraken **Spot** is a different venue surface — different host, different
symbology, different message shapes — and nothing in this directory or in the
adapter touches it.

## Websocket — `wss://futures.kraken.com/ws/v1`

Three sessions, each subscribe-then-listen.

| file | source |
|---|---|
| `ws_info.json` | the unsolicited `{"event":"info","version":1}` every connection opens with |
| `ws_subscribed.json` | the `ticker` acknowledgement of a subscribe |
| `ws_ticker_pf.json` | first `ticker` push for `PF_XBTUSD` |
| `ws_ticker_pi.json` | first `ticker` push for `PI_XBTUSD`, same session |
| `ws_trade.json` | a live `trade` for `PF_XBTUSD` |
| `ws_trade_snapshot.json` | the `trade_snapshot` that preceded it — 100 prints, untrimmed |
| `ws_book_snapshot.json` | the `book_snapshot` for `PF_XBTUSD`, untrimmed |
| `ws_book_delta.json`, `ws_book_delta_next.json` | the next two `book` deltas |
| `ws_alert_bad_product.json` | a second session subscribing to `NOT_A_PRODUCT` |
| `ws_alert_bad_feed.json` | the same session subscribing to a feed that does not exist |

Four things these bytes settle that the documentation does not.

The refusal event is **`alert`**, not `error`, and it carries a `message` and no
code of any kind — there is nothing numeric to classify on, so
`{"event":"alert","message":"Couldn't subscribe to invalid product ..."}` is the
whole verdict. Reading it as an ack turns a rejected feed into a quiet one.

`ws_ticker_pf.json` and `ws_ticker_pi.json` came off **one socket**, and their
`relative_funding_rate` fields are spelled differently — `0.00001477885` against
`8.907125e-6`. That is why the adapter carries its own exponent-aware decimal
parser instead of `parse_decimal_e9`.

The three `book` files are **consecutive frames off one socket**, so the `seq`
chain across them is real rather than constructed: 103307647 → 103307648 →
103307649. `ws_book_snapshot.json` is untrimmed on purpose — it carries 1751
bids and 1360 asks, which is what proves Kraken pushes the *whole* book rather
than a fixed depth, and is the measurement `k_max_levels = 4096` comes from. A
trimmed snapshot would have proved nothing about the ceiling.

Snapshot and delta are **different shapes**, not one shape with a flag: the
snapshot carries `bids`/`asks` arrays of `{price,qty}` objects, while a delta is
a single level at the root with its own `side`.

Not captured: `ticker_lite`. The feed exists and `parse_ticker` reads it — it is
the `ticker` frame with the derivative fields removed, which is why those fields
are optional at the wire layer — but the adapter never subscribes to it, since
dropping mark, index and funding would cost the entire reason this venue is
here.

## REST — `https://futures.kraken.com/derivatives/api/v3`

| file | source |
|---|---|
| `rest_instruments.json` | `GET /instruments` (298 rows) trimmed to six |
| `rest_instruments_live.json` | `GET /instruments` 2026-09-25, all 299 rows, each cut to the fields the parser reads |
| `rest_ticker.json` | `GET /tickers/PF_XBTUSD` |
| `rest_error_bad_symbol.json` | `GET /orderbook?symbol=` — a real `invalidArgument` body |
| `rest_error_auth.json` | `GET /accounts` with no credentials |
| `rest_error_not_found.json` | `GET /nonexistent` |

The three error files are three different things, and an adapter that handles
only the first is wrong twice.

`rest_error_bad_symbol.json` is the documented shape, and its code is a **string
name with a field suffix** — `"invalidArgument: symbol"`. Classification that
matches the whole string puts every real refusal in the default bucket.

`rest_error_auth.json` arrived under **HTTP 200**. A status-code check would
read an authentication failure as a success.

`rest_error_not_found.json` has no `error` field at all — the routing layer
answers `{"status":"NOT_FOUND","result":"error","errors":[{"code":0,...}]}`,
which is why the envelope parser falls back to `status`.

There is no clock endpoint anywhere in this API. Every response carries
`serverTime` as an **ISO-8601 string**, and its fractional width varies within a
single session (`...:16.918Z` and `...:08.64Z` both appear in these captures),
so it is parsed rather than sliced.

### `rest_instruments.json`

Rows were **selected**, not written: each is a verbatim row from the live
response, re-emitted inside the envelope the venue sent, with all the nested
`marginLevels`/`marginSchedules`/`platformsPermitted` bulk left in — the parser
has to skip over that, and a trimmed row would not exercise it.

- `PF_XBTUSD` — perpetual, **linear** (multi-collateral, USD-margined). Tick 1,
  `contractValueTradePrecision` 4, so a quantity step of 0.0001 **BTC**.
- `PI_XBTUSD` — perpetual, **inverse**. Same underlying, tick 0.5, precision 0,
  so a quantity step of 1 **USD of notional**. Both load, both must keep their
  own symbol: merging them prices BTC quantities against a book quoted in USD.
- `PF_TURBOUSD` — precision **-2**, a step of 100. Negative precision means
  coarser than 1, not finer.
- `FI_XBTUSD_260925` — a **dated** future, and `type` does not say so:
  `futures_inverse` covers both `PI_` and `FI_`, exactly as `flexible_futures`
  covers both `PF_` and `FF_`. Only `lastTradingTime` separates them, and it is
  present on exactly the dated rows across all 298. Loading it as a perp is a
  position bug that surfaces at settlement.
- `PF_PEPEUSD` — tick `0.0000000001`, finer than `FixedE9` holds. Refused under
  the no-rounding rule; two rows of 298 are in this state today.
- `PF_SOLUSD` — **the one derived value in this directory.** No live row carries
  `tradeable: false`; Kraken drops delisted contracts from `/instruments`
  outright rather than flagging them. This is a verbatim `PF_SOLUSD` row with a
  single textual substitution, `"tradeable":true` → `"tradeable":false`, so the
  mapper's refusal of an untradeable row is covered by something. Every other
  byte in this directory is the venue's.

A loader that accepts all six is wrong in a way that stays invisible for months.

## Authenticated surfaces — **documentation-derived, not captured**

Everything above is the venue's own bytes off a live socket. The files below
are **not**, and the distinction is the whole point of this section: order
entry, the reconciliation pulls and the private feeds all require credentials,
and this repository has none. Capturing them would mean placing real orders
with real money on a production account.

Each file is the **example payload from Kraken's own reference documentation**
for that endpoint or feed, transcribed verbatim except for the substitutions
listed below. That makes them evidence about *shape* — which key a verdict
hides under, which of two spellings an id takes, whether a size is what is left
or what was asked for — and evidence about nothing else. A field Kraken adds
tomorrow will not appear here, and no assertion in this directory should be
read as proof that the live wire matches.

| file | source |
|---|---|
| `rest_send_order_placed.json` | `POST /sendorder` success example |
| `rest_send_order_rejected.json` | `POST /sendorder` refusal example |
| `rest_cancel_order.json` | `POST /cancelorder` example |
| `rest_edit_order.json` | `POST /editorder` example |
| `rest_open_orders.json` | `GET /openorders` example |
| `rest_fills.json` | `GET /fills` example |
| `rest_open_positions.json` | `GET /openpositions` example |
| `ws_challenge.json` | the `{"event":"challenge"}` reply to a challenge request |
| `ws_open_orders_snapshot.json` | `open_orders_snapshot` example |
| `ws_open_orders_new.json` | `open_orders` delta carrying an `order` object |
| `ws_open_orders_partial.json` | the same delta with `reason: "partial_fill"` |
| `ws_open_orders_cancel.json` | `open_orders` removal — an id, `is_cancel` and a reason |
| `ws_open_orders_post_refused.json` | the same removal with a refusal reason |
| `ws_fills_snapshot.json` | `fills_snapshot` example |
| `ws_fills_partial.json` | a `fills` delta with a non-zero `remaining_order_qty` |

### The substitutions, and why each one was made

The documentation's examples name instruments and quantities that this
directory's catalogue does not contain, and a fixture whose symbol is absent
from `rest_instruments.json` proves only that the mapper drops unknown symbols.
Every change below is textual and listed so a reader can undo it:

- **`PF_XBTUSD` → `PI_XBTUSD` in the websocket order and fill frames.** The
  docs use `PF_XBTUSD` with `qty: 1000`, which is 1000 **BTC** against that
  contract's 0.0001 step. `PI_XBTUSD` counts contracts of 1 USD, so the same
  numbers are a plausible position and every price still lands on its 0.5 tick.
- **`FI_XBTUSD_201225` → `FI_XBTUSD_260925`** in `rest_open_positions.json`, so
  the dated row matches the one in `rest_instruments.json`. It is refused by the
  catalogue either way; matching it makes the refusal about the *market type*
  rather than about an expiry that no longer exists.
- **`PF_DEFIUSD` → `PF_XBTUSD`, price 570 → 64000, size 1 → 0.5** in the same
  file. This is the one row whose average entry *does* divide by its tick, and
  it is the only reason the file can show both outcomes: a size kept with an
  average, and a size kept without one.
- **A `cliOrdId` was added** to one `openorders` row, one `fills` row and the
  websocket order and fill deltas. Kraken's examples omit it — the field is
  optional and their example orders carried none — but it is the id the OMS
  routes on, so a fixture set in which it never appears cannot test routing.
  The value is a real 26-character Crockford ULID.
- **A third `openorders` row** naming `NOT_A_PRODUCT`, and a third `fills` row
  with `fillType: "liquidation"`. Both are additions, not edits: the first pins
  that a symbol we never loaded yields no report rather than a report against
  instrument zero, the second that a forced fill is filed under `Unknown`
  liquidity rather than under a fee tier it did not earn.
- **`ws_open_orders_partial.json` and `ws_fills_partial.json` are constructed**
  from the corresponding documented frame by changing `qty`/`filled` and
  `remaining_order_qty` only. Kraken documents no partial-fill example, and
  "`qty` is what REMAINS" is the single most expensive thing to get wrong on
  this feed — reading it as the original size turns a half-filled order into a
  smaller order that never filled.

`rest_open_positions.json` keeps the documentation's own
`price: 9392.749993345933` untouched. That is a full-precision average of fills
that no tick size divides, and it is why `map_position` reports the size while
leaving the average undefined: dropping the row would tell a reconciler we are
flat while we are short 10000 contracts.

The signing test uses no fixture at all. Its expected `Authent` values were
computed independently (base64-decode, SHA-256, HMAC-SHA-512, base64-encode)
rather than read from any capture, which is the only way a signature test proves
anything about the implementation under it.
