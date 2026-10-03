# backpack fixtures

Captured from production Backpack Exchange on 2026-09-24, public endpoints, no
credentials. REST is `https://api.backpack.exchange`; the socket is the bare
host `wss://ws.backpack.exchange/` (`/stream` answers 400). Socket frames are
verbatim; REST bodies are trimmed to a few rows.

| file | source |
|---|---|
| `rest_markets.json` | `GET /api/v1/markets`, trimmed |
| `rest_tickers.json` | `GET /api/v1/tickers`, trimmed |
| `rest_depth.json` | `GET /api/v1/depth`, trimmed |
| `rest_error.json` | a REST request naming an unknown market symbol |
| `ws_book_ticker.json` | `bookTicker`, SOL_USDC |
| `ws_depth.json`, `ws_depth_next.json` | `depth`, SOL_USDC_PERP, two consecutive frames of one session |
| `ws_mark_price_perp.json`, `ws_mark_price_spot.json` | `markPrice`, SOL_USDC_PERP and SOL_USDC |
| `ws_trade.json` | `trade`, SOL_USDC_PERP |
