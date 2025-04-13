# Coinbase fixtures

All captured live from production on 2026-08-10, unedited except where a file
says otherwise. Compact single-line JSON, one frame per file.

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
| `ws_l2update.json` | A `level2_batch` delta, trimmed to a short `changes` list. The adapter does **not** subscribe to depth (the channel's opening snapshot measured 45,440 levels in one 1.14 MiB frame, over `WsConfig::max_message_bytes`); this is kept so a test pins that such a frame is ignored rather than fatal. |
