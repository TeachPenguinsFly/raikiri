# Coinbase fixtures

Captured live from production Coinbase Exchange on 2026-08-10 unless a row
says otherwise. Files are compact single-line JSON, one frame per file, and
unedited unless a row says so. REST is `https://api.exchange.coinbase.com`
(Coinbase Exchange, not Advanced Trade on `api.coinbase.com`); public reads need
no credentials. The socket is `wss://ws-feed.exchange.coinbase.com`, captured
with

```
{ echo '<subscribe>'; sleep 20; } | websocat -B 900000 "wss://ws-feed.exchange.coinbase.com" | head -60
```

## REST

| file | source |
| --- | --- |
| `rest_products.json` | `GET /products`, trimmed to 4 rows: `BTC-USD`, `ETH-USD`, `SYND-USD` (`limit_only`) and `DDX-EUR` (`delisted`, `trading_disabled`); the rows are otherwise byte-identical |
| `rest_product_btc.json` | `GET /products/BTC-USD` |
| `rest_time.json` | `GET /time` |
| `rest_book_level1.json` | `GET /products/BTC-USD/book?level=1` |
| `rest_error_not_found.json` | `GET /products/NOPE-XYZ`, HTTP 404 |

## WebSocket

| file | source |
| --- | --- |
| `ws_subscriptions.json` | the confirmation for `{"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker","matches"]}` |
| `ws_subscriptions_level2_renamed.json` | the confirmation for a subscribe asking for `level2_batch`, which comes back named `level2_50` |
| `ws_subscriptions_empty.json` | the empty confirmation (`"channels":[]`) that arrives beside `ws_error.json` |
| `ws_error.json` | the refusal of a subscribe to product `NOPE-XYZ` |
| `ws_ticker.json` | `ticker`, BTC-USD |
| `ws_match.json` | `matches`, BTC-USD, the print with the same `trade_id` (1068956434) as `ws_ticker.json` |
| `ws_match_maker_buy.json` | `matches`, BTC-USD, a print with `side` `"buy"` from the same session |
| `ws_last_match.json` | the replay print sent when a `matches` subscription opens |
| `ws_l2update.json` | a `level2_batch` delta, trimmed to a short `changes` list |
| `ws_l2_snapshot.json` | the `level2_batch` opening snapshot, captured 2026-08-17, trimmed to the first 150 levels a side by splicing at the bracket boundaries |
| `ws_l2update_chained.json` | the next frame of that capture, untrimmed |

## Private / order entry

No credentials exist for this repository, so these bodies are constructed from
`docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/exchange-spec.yaml`
(the `apiOrder` schema, read 2026-08-26), with prices and sizes on the BTC-USD
increments in `rest_products.json`. The `client_oid` is the UUID the adapter
derives from the ULID `01K3QW9F2H6M5N8P0R2T4V6X8Z`. One file is a live capture.

| file | source |
| --- | --- |
| `rest_error_unauthorized.json` | verbatim: `GET /orders?status=open` with no credentials, 2026-08-26, HTTP 401 |
| `rest_order_ack.json` | constructed `POST /orders` answer for a resting post-only limit buy, `status: "pending"` |
| `rest_order_rejected.json` | constructed: the same order with `status: "rejected"`, `reject_reason: "post only"` |
| `rest_orders_open.json` | constructed `GET /orders`: a partially filled `BTC-USD` order and an `ETH-USD` order with an empty `client_oid` |
| `rest_order_done_canceled.json` | constructed: `status: "done"`, `done_reason: "canceled"`, 0.2 of 0.5 filled |
| `rest_order_done_filled.json` | constructed: `status: "done"`, `done_reason: "filled"` |
| `rest_order_market.json` | constructed: a market order, with no `price` field |
