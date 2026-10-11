# lighter fixtures

Captured from mainnet on 2026-09-24, public endpoints, no credentials. REST is
`https://mainnet.zklighter.elliot.ai`; the socket is
`wss://mainnet.zklighter.elliot.ai/stream`. Trimmed fixtures had rows cut out of
the original text without re-serialising, so every number keeps the venue's
spelling.

| file | source |
|---|---|
| `rest_order_book_details.json` | `GET /api/v1/orderBookDetails`, trimmed |
| `rest_order_book_details_tradfi.json` | the same, 2026-10-05: a stock, an index, a korean share in dollars, a metal, a currency, the QNT coin filed with the stocks, a pre-ipo share and an unnamed group |
| `ws_order_book_snapshot.json`, `ws_order_book_update.json`, `ws_order_book_update_next.json` | `order_book:2054`, the snapshot and the two updates after it in one session; the nonce chain holds across all three |
| `ws_market_stats.json` | `market_stats:1` |
| `ws_market_stats_all.json` | `market_stats:all` |
| `ws_spot_market_stats.json` | `spot_market_stats:2048` |
| `ws_trade_snapshot.json`, `ws_trade_update.json` | `trade:1`: the snapshot sent on subscribe, then one update |
| `ws_error_rate_limit.json` | code 30009, sent past the IP's socket message limit |
| `ws_error_too_many_subscriptions.json` | code 23001, sent past the subscription limit |
| `rest_trades.json` | `GET /api/v1/recentTrades?market_id=1&limit=1`, 2026-10-11; the row an account's `/api/v1/trades` returns, fees omitted at zero |
| `rest_account.json` | `GET /api/v1/account?by=index&value=281474976710654` (the public pool), 2026-10-11, two positions kept and re-serialised |
| `rest_next_nonce.json` | `GET /api/v1/nextNonce?account_index=1&api_key_index=0` |
| `rest_send_tx_api_key_not_found.json` | testnet `POST /api/v1/sendTx`, a create signed by the reference signer for an account with no key |
| `rest_send_tx_invalid_signature.json`, `ws_send_tx_invalid_signature.json` | a cancel with a junk signature, by rest and by `jsonapi/sendtx` with id 43 |
| `ws_account_auth_expired.json` | `account_all_orders/1` subscribed with an expired token |
| `ws_account_all_trades_subscribed.json` | `account_all_trades/1`, the empty snapshot |
