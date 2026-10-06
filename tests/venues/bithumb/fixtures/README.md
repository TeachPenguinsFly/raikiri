# Bithumb fixtures

Captured 2026-10-06 around 17:00 UTC from production, unauthenticated: REST with `urllib`,
the socket with `websocat` against `wss://ws-api.bithumb.com/websocket/v1`. Frames are verbatim,
one per file. The two catalogue bodies keep seven of the venue's markets; nothing inside a kept
row was retyped. Bithumb serves the Upbit v1 format, so the parsers are Upbit's.

## REST, `GET https://api.bithumb.com`

| file | source |
| --- | --- |
| `rest_market_all.json` | `/v1/market/all?isDetails=true`; rows `KRW-BTC`, `KRW-ETH`, `KRW-XRP`, `KRW-SHIB`, `KRW-USDT`, `BTC-ETH`, `BTC-XRP` |
| `rest_ticker.json` | `/v1/ticker?markets=` the same seven; `timestamp` and `trade_timestamp` run nine hours ahead |
| `rest_orderbook_krw_btc.json` | `/v1/orderbook?markets=KRW-BTC&count=1`, which answers 30 levels regardless |
| `rest_error_unknown_market.json` | `/v1/ticker?markets=KRW-NOPEX`, refused with 404 |

## WebSocket, `wss://ws-api.bithumb.com/websocket/v1`

Subscribed with `[{"ticket":"raikiri"},{"type":"orderbook","codes":[...]},{"type":"trade","codes":[...]},{"format":"DEFAULT"}]`.
Book stamps are in microseconds, trade stamps in milliseconds.

| file | source |
| --- | --- |
| `ws_orderbook_krw_btc.json` | `orderbook` `KRW-BTC`, a realtime frame with two empty levels |
| `ws_orderbook_snapshot_usdt.json` | `orderbook` `KRW-USDT`, the snapshot the subscription opens with |
| `ws_orderbook_btc_eth.json` | `orderbook` `BTC-ETH` |
| `ws_orderbook_krw_shib.json` | `orderbook` `KRW-SHIB` |
| `ws_trade_snapshot.json` | `trade`, the snapshot print the subscription opens with |
| `ws_trade_realtime.json` | `trade` `KRW-USDT`, realtime |
| `ws_trade_krw_xrp.json` | `trade` `KRW-XRP` |
| `ws_error.json` | answer to a request naming the type `orderbok` |
