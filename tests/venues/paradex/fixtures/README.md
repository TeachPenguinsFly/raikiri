# Paradex fixtures

Captured 2026-10-06 against production, unauthenticated: REST with `curl` from
`https://api.prod.paradex.trade/v1`, the socket with a small node client against
`wss://ws.api.prod.paradex.trade/v1?sbeSchemaId=1&sbeSchemaVersion=1`. The socket answers
subscriptions in json-rpc text and sends the data as binary SBE (schema 1, version 1, layout in
`paradex_1_0.xml` of the venue's python sdk). Each `.bin` file is one binary frame, byte for byte.
The catalogue body keeps eight of its 8724 rows; nothing inside a kept row was retyped.

## REST

| file | source |
| --- | --- |
| `rest_markets.json` | `GET /v1/markets`; rows `BTC-USD-PERP`, `ETH-USD-PERP`, `kPEPE-USD-PERP`, `XAU-USD-PERP`, `NVDA-USD-PERP`, `US500-USD-PERP`, the spot pair `ETH-USD` and one dated option |
| `rest_time.json` | `GET /v1/system/time` |

## WebSocket

| file | source |
| --- | --- |
| `ws_reply_subscribed.json` | answer to `subscribe` `order_book.BTC-USD-PERP.deltas` |
| `ws_reply_error.json` | answer to `subscribe` `bogus.BTC-USD-PERP` |
| `sbe_book_snapshot.bin` | the first `order_book.ETH-USD-PERP.deltas` frame, the whole api book |
| `sbe_book_delta.bin` | an `order_book.ETH-USD-PERP.deltas` delta that deletes a bid |
| `sbe_book_delta_empty.bin` | an `order_book.ETH-USD-PERP.deltas` delta with no levels |
| `sbe_bbo.bin` | `bbo.ETH-USD-PERP` |
| `sbe_trade.bin` | `trades.ALL`, a `BTC-USD-PERP` fill |
| `sbe_trade_rpi.bin` | `trades.ALL`, a `US500-USD-PERP` rpi fill |
| `sbe_trade_option.bin` | `trades.ALL`, a block trade in a `US500` put |
| `sbe_summary.bin` | `markets_summary.BTC-USD-PERP` |
| `sbe_funding.bin` | `funding_data.ALL`, the `BTC-USD-PERP` row |
