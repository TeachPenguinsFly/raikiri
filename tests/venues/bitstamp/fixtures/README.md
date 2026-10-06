# Bitstamp fixtures

Captured 2026-10-06 around 17:01 UTC from production, unauthenticated: REST with `urllib`,
the socket with `websocat` against `wss://ws.bitstamp.net`. Frames are verbatim, one per file.
The two catalogue bodies keep fourteen of the venue's markets; nothing inside a kept row was retyped.

## REST, `GET https://www.bitstamp.net`

| file | source |
| --- | --- |
| `rest_markets.json` | `/api/v2/markets/`; spot `btcusd`, `btceur`, `ethbtc`, `eurusd`, `usdtusd` and perpetuals `btcusd-perp`, `goldusd-perp`, `silverusd-perp`, `paxgusd-perp`, `qqqusd-perp`, `ewyusd-perp`, `eurusd-perp`, `wtiusd-perp`, `brentusd-perp` |
| `rest_ticker.json` | `/api/v2/ticker/`, the same fourteen |

## WebSocket, `wss://ws.bitstamp.net`

Each channel subscribed with `{"event":"bts:subscribe","data":{"channel":"..."}}`.

| file | source |
| --- | --- |
| `ws_order_book_btcusd.json` | `order_book_btcusd` |
| `ws_order_book_usdtusd.json` | `order_book_usdtusd`, a thin book of 24 bids |
| `ws_order_book_btcusd_perp.json` | `order_book_btcusd-perp` |
| `ws_order_book_ewyusd_perp.json` | `order_book_ewyusd-perp` |
| `ws_live_trades_btcusd.json` | `live_trades_btcusd` |
| `ws_live_trades_ethusd.json` | `live_trades_ethusd` |
| `ws_funding_rate_btcusd_perp.json` | `funding_rate_btcusd-perp` |
| `ws_funding_rate_goldusd_perp.json` | `funding_rate_goldusd-perp` |
| `ws_subscription_succeeded.json` | answer to the `order_book_btcusd` subscription |
| `ws_error.json` | answer to the channel `bbo_btcusd`, which does not exist |
