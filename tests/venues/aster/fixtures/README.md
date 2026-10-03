# aster fixtures

Captured live on 2026-09-20 from production Aster USD-margined perpetuals,
REST on `https://fapi.asterdex.com` and the socket on
`wss://fstream.asterdex.com/stream`. Every path is public; no credentials.

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.asterdex.com/stream?streams=btcusdt@bookTicker` |
| `ws_agg_trade.json` | `btcusdt@aggTrade` |
| `ws_mark_price.json` | `btcusdt@markPrice@1s` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two consecutive frames of one session (`u` 554245272553 is the next frame's `pu`) |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed to three symbols: one plain USDT perp, one USD1-quoted, one not yet trading |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_funding_info.json` | `GET /fapi/v1/fundingInfo`, trimmed to three rows |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_ticker_24h.json` | `GET /fapi/v1/ticker/24hr?symbol=BTCUSDT` |
| `rest_server_time.json` | `GET /fapi/v1/time` |
