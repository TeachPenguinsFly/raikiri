# Bullish fixtures

Captured 2026-10-06 against production (`api.exchange.bullish.com`), unauthenticated: REST with
`curl`, the sockets with a small node client. Socket frames are verbatim, one per file. The
catalogue body keeps nine of its 1664 rows: three spot pairs, three enabled perpetuals, a disabled
perpetual, a dated future and an option; nothing inside a kept row was retyped.

## REST, `https://api.exchange.bullish.com/trading-api`

| file | source |
| --- | --- |
| `rest_markets.json` | `GET /v1/markets` |
| `rest_time.json` | `GET /v1/time` |
| `rest_index_prices.json` | `GET /v1/index-prices`; rows BTC, CD20, ETH, SOL and USDC of 101 |

## WebSocket

| file | source |
| --- | --- |
| `ws_l2_snapshot_perp.json` | `/v1/market-data/orderbook`, `l2Orderbook` `BTC-USDC-PERP`, the first frame |
| `ws_l2_next_perp.json` | the frame after it, whose sequence range starts where the first ended |
| `ws_l2_snapshot_spot.json` | `/v1/market-data/orderbook`, `l2Orderbook` `BTCUSDC` |
| `ws_refusal.json` | `/v1/market-data/orderbook`, `l2Orderbook` for the unknown symbol `NOPE` |
| `ws_tick_perp.json` | `/v1/market-data/tick`, `tick` `BTC-USDC-PERP` |
| `ws_tick_spot.json` | `/v1/market-data/tick`, `tick` `BTCUSDC` |
| `ws_reply_subscribed.json` | answer to the `tick` subscribe |
| `ws_trades_snapshot.json` | `/v1/market-data/trades`, `anonymousTrades` `BTCUSDC`, the history sent on subscribe |
| `ws_trades_update.json` | the same stream, a live print |
| `ws_keepalive_pong.json` | answer to `keepalivePing` |
