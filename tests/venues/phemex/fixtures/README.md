# phemex fixtures

Captured live on 2026-09-25 from `api.phemex.com` and `wss://ws.phemex.com`, no
credentials. Trimmed fixtures were cut down with Python's `json` module, which
keeps every string and integer as the venue sent it.

| file | source |
|---|---|
| `rest_products.json` | `GET /public/products`, trimmed to currencies BTC, USDT, USDC, ETH, SHIB; `products` sBTCUSDT, sSHIBUSDT (base valueScale 4), BTCUSD and cETHUSD (inverse), one delisted spot and one delisted perp; `perpProductsV2` BTCUSDT (8 h), CAKEUSDT (4 h, description still says 8 h), XAUUSDT (TradFi, 4 h), u1000SHIBUSDT (base "1000 SHIB") and one delisted row |
| `ws_real_book_snapshot.json`, `ws_real_book_update.json`, `ws_real_book_resnapshot.json` | `orderbook_p` BTCUSDT: the subscribe snapshot, the first incremental after it, and the snapshot the venue resent ~60 s later in the same session |
| `ws_spot_book_snapshot.json`, `ws_spot_book_update.json` | `orderbook` sBTCUSDT, snapshot and first incremental |
| `ws_inverse_book_snapshot.json` | `orderbook` BTCUSD, snapshot |
| `ws_real_trades_snapshot.json` | `trade_p` BTCUSDT subscribe backlog, trimmed from 200 prints to 2 |
| `ws_real_trades.json`, `ws_spot_trades.json`, `ws_inverse_trades.json` | the first incremental of `trade_p` BTCUSDT, `trade` sBTCUSDT and `trade` BTCUSD |
| `ws_perp_pack_snapshot.json` | `perp_market24h_pack_p` snapshot, trimmed from ~320 rows to BTCUSDT, CAKEUSDT, XAUUSDT, LSKUSDT (trading, absent from the catalogue) and one all-zero delisted row |
| `ws_perp_pack_update.json` | the first incremental after it, trimmed to 3 rows; it carries no `fields` |
| `ws_inverse_ticker.json`, `ws_spot_ticker.json` | one `market24h` (BTCUSD) and one `spot_market24h` (sBTCUSDT) frame |
| `ws_ack.json`, `ws_pong.json` | the reply to a subscription and to `server.ping` |
| `ws_error_subscription_limit.json` | the reply to the 102nd subscription on one socket, as printed by the limit probe |
