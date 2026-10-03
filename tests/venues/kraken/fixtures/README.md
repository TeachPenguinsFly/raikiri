# Kraken Futures fixtures

The public files were captured from production Kraken Futures on 2026-08-10,
unless a row gives another date, with no credentials. Frames were copied out
verbatim without re-serialising, because Kraken spells numbers differently per
contract (`0.00001477885` against `8.907125e-6`) and a JSON round trip would
normalise them. The authenticated files at the end are Kraken's documented
examples, not captures.

## Websocket: `wss://futures.kraken.com/ws/v1`

Three subscribe-then-listen sessions.

| file | source |
|---|---|
| `ws_info.json` | the unsolicited `{"event":"info","version":1}` sent on connect |
| `ws_subscribed.json` | the acknowledgement of a `ticker` subscribe |
| `ws_ticker_pf.json` | first `ticker` push for `PF_XBTUSD` |
| `ws_ticker_pi.json` | first `ticker` push for `PI_XBTUSD`, same socket |
| `ws_trade.json` | a live `trade` for `PF_XBTUSD` |
| `ws_trade_snapshot.json` | the `trade_snapshot` that preceded it, 100 prints, untrimmed |
| `ws_book_snapshot.json` | the `book_snapshot` for `PF_XBTUSD`, untrimmed |
| `ws_book_delta.json`, `ws_book_delta_next.json` | the next two `book` deltas on the same socket (`seq` 103307647 → 103307648 → 103307649) |
| `ws_alert_bad_product.json` | a second session subscribing to `NOT_A_PRODUCT` |
| `ws_alert_bad_feed.json` | the same session subscribing to a feed that does not exist |

## REST: `https://futures.kraken.com/derivatives/api/v3`

| file | source |
|---|---|
| `rest_instruments.json` | `GET /instruments`, trimmed to six verbatim rows inside the original envelope (see below) |
| `rest_instruments_live.json` | `GET /instruments` on 2026-09-25, all 299 rows, each cut to the fields the parser reads |
| `rest_ticker.json` | `GET /tickers/PF_XBTUSD` |
| `rest_error_bad_symbol.json` | `GET /orderbook?symbol=` |
| `rest_error_auth.json` | `GET /accounts` with no credentials (HTTP 200) |
| `rest_error_not_found.json` | `GET /nonexistent` |

`rest_instruments.json` keeps `PF_XBTUSD`, `PI_XBTUSD`, `PF_TURBOUSD`,
`FI_XBTUSD_260925`, `PF_PEPEUSD` and `PF_SOLUSD` with their nested
`marginLevels`/`marginSchedules`/`platformsPermitted` intact. `PF_SOLUSD` has
one edit: `"tradeable":true` → `"tradeable":false`, since no live row carries
`false`.

## Authenticated: documented examples

Order entry, the reconciliation pulls and the private feeds need credentials,
so these files are the example payloads from Kraken's reference documentation,
transcribed verbatim except for the substitutions listed after the table.

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
| `ws_open_orders_cancel.json` | `open_orders` removal: an id, `is_cancel` and a reason |
| `ws_open_orders_post_refused.json` | the same removal with a refusal reason |
| `ws_fills_snapshot.json` | `fills_snapshot` example |
| `ws_fills_partial.json` | a `fills` delta with a non-zero `remaining_order_qty` |

Substitutions, all textual:

- `PF_XBTUSD` → `PI_XBTUSD` in the websocket order and fill frames.
- `FI_XBTUSD_201225` → `FI_XBTUSD_260925` in `rest_open_positions.json`.
- `PF_DEFIUSD` → `PF_XBTUSD`, price 570 → 64000, size 1 → 0.5, in the same file.
  Its other row keeps the documented `price: 9392.749993345933`.
- A `cliOrdId` (a 26-character Crockford ULID) added to one `openorders` row,
  one `fills` row and the websocket order and fill deltas.
- A third `openorders` row naming `NOT_A_PRODUCT`, and a third `fills` row with
  `fillType: "liquidation"`.
- `ws_open_orders_partial.json` and `ws_fills_partial.json` built from the
  documented frames by changing only `qty`/`filled` and `remaining_order_qty`;
  Kraken documents no partial fill.

The signing test uses no fixture. Its expected `Authent` values were computed
independently (base64-decode, SHA-256, HMAC-SHA-512, base64-encode).
