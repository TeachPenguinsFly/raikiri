# Coinbase fixtures

All captured live from production on 2026-08-10, unedited except where a file
says otherwise. Compact single-line JSON, one frame per file.

The exception is the **Private / order entry** section at the bottom: no
credentials exist for this repository, so those bodies are constructed from the
published OpenAPI specification rather than captured. One of them is a live
capture and says so.

REST base `https://api.exchange.coinbase.com` (Coinbase **Exchange**, the
former Coinbase Pro — not Advanced Trade on `api.coinbase.com`). Public reads
need no credentials.

WS `wss://ws-feed.exchange.coinbase.com`.

## REST

| file | provenance |
| --- | --- |
| `rest_products.json` | `GET /products`, **trimmed** from 832 rows to 4: `BTC-USD` and `ETH-USD` (both `online`), `SYND-USD` (`online` but `limit_only`, which the mapper must still accept — the flag gates order entry, not market data), and `DDX-EUR` (`delisted` + `trading_disabled`, which the mapper must refuse). Rows are otherwise byte-identical to the response. |
| `rest_product_btc.json` | `GET /products/BTC-USD`. Note it is the bare object, not a one-element array. |
| `rest_time.json` | `GET /time`. `iso` has **3** fractional digits here; `epoch` beside it is a JSON float and is deliberately not read. |
| `rest_book_level1.json` | `GET /products/BTC-USD/book?level=1`. Its `time` has **9** fractional digits — the same API disagrees with itself on precision, which is why the ISO parser takes a variable-length fraction. |
| `rest_error_not_found.json` | `GET /products/NOPE-XYZ` → HTTP 404. The whole body: no code, no envelope. The status is the only machine-readable part of a Coinbase failure. |

## WebSocket

Captured with
`{ echo '<subscribe>'; sleep 20; } | websocat -B 900000 "wss://ws-feed.exchange.coinbase.com" | head -60`.

| file | provenance |
| --- | --- |
| `ws_subscriptions.json` | Confirmation for `{"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker","matches"]}`. |
| `ws_subscriptions_level2_renamed.json` | Confirmation for a subscribe that asked for `level2_batch`; it comes back named **`level2_50`**. Kept because matching an ack against the requested name would read a confirmed channel as a refused one. |
| `ws_subscriptions_empty.json` | The confirmation that arrives *beside* `ws_error.json`: `"channels":[]`. An empty ack is what a connection whose every subscription was refused looks like. |
| `ws_error.json` | Deliberately provoked by subscribing to product `NOPE-XYZ`. A refused subscription is **not** silent. `message` is the same text on every refusal; `reason` is the part that identifies what died. |
| `ws_ticker.json` | `ticker` frame for BTC-USD. |
| `ws_match.json` | The `match` for the **same `trade_id` (1068956434)** as `ws_ticker.json`. Its `side` is `"sell"` and its price `64095.98` equals that ticker's `best_ask` — a print at the ask is a print a *taker who bought* caused, so `side` is the **maker's**. The paired ticker's own `side` for that trade is `"buy"`. Joining all 21 prints in the capture by `trade_id`, the two fields disagreed 21 times out of 21. |
| `ws_match_maker_buy.json` | The other side of the same fact: `side` `"buy"` at `64095.97`, the paired ticker's `best_bid`. |
| `ws_last_match.json` | The one replay print Coinbase sends when a `matches` subscription opens. Same shape, different `type`. |
| `ws_l2update.json` | A `level2_batch` delta, trimmed to a short `changes` list. |
| `ws_l2_snapshot.json` | The `level2_batch` opening snapshot, captured 2026-08-17. **Trimmed** from 18,427 bids / 27,378 asks to the first 150 a side by splicing at the bracket boundaries; every number is still spelled the way Coinbase spelled it. The whole frame was 1,203,894 bytes, which is why this venue raises `WsConfig::max_message_bytes` from the 1 MiB default. |
| `ws_l2update_chained.json` | The very next frame of that capture, untrimmed. Sixteen changes: five deletes, ten restatements, and a bid at 56015.12 that sits below the floor of the trimmed snapshot — the band case. |

## Private / order entry

Constructed from `docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/exchange-spec.yaml`
(the `apiOrder` schema, read 2026-08-26), with values chosen to agree with
`rest_products.json`: every price is a multiple of BTC-USD's `0.01`
`quote_increment` and every size a multiple of its `0.00000001`
`base_increment`, so an order in one of these files really would be accepted.
The `client_oid` is the UUID the adapter derives from the ULID
`01K3QW9F2H6M5N8P0R2T4V6X8Z`; see `rest.hpp` for why that is a conversion.

| file | provenance |
| --- | --- |
| `rest_error_unauthorized.json` | **Verbatim.** `GET /orders?status=open` with no credentials, 2026-08-26. HTTP 401, and the body really is only `{"message":"Unauthorized."}` — the status line is the whole machine-readable part. |
| `rest_order_ack.json` | Constructed. `POST /orders` answering a resting post-only limit buy: `status: "pending"`, `filled_size: "0"`. The venue returns the whole order object, which is why acceptance needs no stream. |
| `rest_order_rejected.json` | Constructed. The same order refused by the engine: `status: "rejected"`, `reject_reason: "post only"`. |
| `rest_orders_open.json` | Constructed. `GET /orders`. Two rows: a partially filled `BTC-USD` order (0.5 ordered, 0.2 filled), and an `ETH-USD` order — **the row the mapper must drop**, because the sweep is account-wide and names products a given process never loaded. Its `client_oid` is empty, which is what an order placed by an earlier process looks like. |
| `rest_order_done_canceled.json` | Constructed. `status: "done"`, `done_reason: "canceled"`, with 0.2 of 0.5 filled — the remainder that a Canceled event must report as left. |
| `rest_order_done_filled.json` | Constructed. `status: "done"`, `done_reason: "filled"`. The row a status report calls Filled and an event deliberately says nothing about, because this adapter has no source for the executions behind it. |
| `rest_order_market.json` | Constructed. A market order, which carries **no `price` field at all** — the absence must stay undefined rather than becoming a zero. |
