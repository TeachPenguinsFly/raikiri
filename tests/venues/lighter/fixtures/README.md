# lighter fixtures

Captured from mainnet on 2026-09-24, public endpoints, no credentials. REST is
`https://mainnet.zklighter.elliot.ai`; the socket is
`wss://mainnet.zklighter.elliot.ai/stream`. Trimmed fixtures had rows cut out of
the original text without re-serialising, so every number keeps the venue's
spelling.

| file | source |
|---|---|
| `rest_order_book_details.json` | `GET /api/v1/orderBookDetails`, trimmed |
| `ws_order_book_snapshot.json`, `ws_order_book_update.json`, `ws_order_book_update_next.json` | `order_book:2054`, the snapshot and the two updates after it in one session; the nonce chain holds across all three |
| `ws_market_stats.json` | `market_stats:1` |
| `ws_market_stats_all.json` | `market_stats:all` |
| `ws_spot_market_stats.json` | `spot_market_stats:2048` |
| `ws_trade_snapshot.json`, `ws_trade_update.json` | `trade:1`: the snapshot sent on subscribe, then one update |
| `ws_error_rate_limit.json` | code 30009, sent past the IP's socket message limit |
| `ws_error_too_many_subscriptions.json` | code 23001, sent past the subscription limit |
