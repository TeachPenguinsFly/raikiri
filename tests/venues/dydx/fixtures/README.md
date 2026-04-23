# dydx fixtures

Captured live on 2026-09-25 from `indexer.dydx.trade` with no credentials.
Frames were written verbatim; trimmed ones were cut by text, so every number
keeps the venue's spelling.

| file | source |
|---|---|
| `rest_perpetual_markets.json` | `GET /v4/perpetualMarkets`, trimmed from 296 rows to BTC (20-decimal rate, off-grid oracle), ETH, TRX (20-decimal rate, 10-decimal oracle), PEPE (tick 1e-10), APE, MATIC and the longest ticker (both FINAL_SETTLEMENT) |
| `ws_connected.json`, `ws_unsubscribed.json` | control frames |
| `ws_book_snapshot.json`, `ws_book_batch.json` | `v4_orderbook` BTC-USD, `batched:true`: the `subscribed` snapshot (100 a side) and the first batch after it, same session |
| `ws_book_snapshot_shallow.json` | `v4_orderbook` APE-USD snapshot, 33 bids and 34 asks |
| `ws_book_update.json` | `v4_orderbook` ETH-USD unbatched `channel_data` |
| `ws_trades_subscribed.json` | `v4_trades` SOL-USD `subscribed` backlog, trimmed from 100 trades to 2 |
| `ws_trades_update.json` | `v4_trades` BTC-USD unbatched `channel_data`, three prints |
| `ws_trades_batch.json` | `v4_trades` ETH-USD `channel_batch_data` |
| `ws_error_limit.json` | the refusal of the 33rd `v4_orderbook` subscription on one socket |
