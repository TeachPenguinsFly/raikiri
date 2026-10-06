# Arcus fixtures

Captured 2026-10-06 against production (`api.arcus.xyz`), unauthenticated: REST with `curl`,
the socket with a small node client against `wss://api.arcus.xyz/v1/ws`. Socket frames are
verbatim, one per file. The two catalogue bodies keep six of the 67 markets (BTC, ETH, GLD,
SPY, NVDA and the offline F) and drop the rest; nothing inside a kept row was retyped.

## REST, `https://api.arcus.xyz`

| file | source |
| --- | --- |
| `rest_markets.json` | `GET /v1/markets` |
| `rest_time.json` | `GET /v1/time` |

## WebSocket, `wss://api.arcus.xyz/v1/ws`

| file | source |
| --- | --- |
| `ws_connected.json` | the greeting on connect |
| `ws_subscribed_book.json` | answer to `l2OrderbookUpdates` `ETH-USD`, carrying the snapshot |
| `ws_book_delta.json` | an `l2OrderbookUpdates` `ETH-USD` delta that deletes a level |
| `ws_subscribed_bbo.json` | answer to `bbo` `BTC-USD`, carrying the current top |
| `ws_bbo.json` | a `bbo` `BTC-USD` frame |
| `ws_subscribed_trades.json` | answer to `trades` `BTC-USD`, which carries no backlog |
| `ws_trades.json` | a `trades` `BTC-USD` frame with one taker's fills |
| `ws_markets.json` | a `markets` snapshot, trimmed to the six markets above |
| `ws_degraded.json` | `degraded` for `l2OrderbookUpdates` `SOL-USD` with `nLevels` 100 |
| `ws_error_unknown_channel.json` | `subscribe` to a channel named `bogus` |
| `ws_error_not_subscribed.json` | `unsubscribe` from a `bbo` that was not held |
