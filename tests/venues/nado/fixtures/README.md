# Nado fixtures

Captured 2026-10-11 from production with no credentials. Every file is a real
response or frame, spliced out verbatim.

The gateway answers 403 to a REST request whose `Accept-Encoding` names none of
gzip, br or deflate, and to a websocket upgrade that does not offer
permessage-deflate. It also refuses Belarusian addresses outright
(`{"reason": "ip", "blocked": true}`); `/v1/query?type=symbols` was the one
path that answered such an address.

## REST

| file | request |
| --- | --- |
| `rest_symbols.json` | `GET https://gateway.prod.nado.xyz/v1/query?type=symbols`, whole: 82 perps and 13 spot |
| `rest_market_liquidity.json` | `GET https://gateway.prod.nado.xyz/v1/query?type=market_liquidity&product_id=2&depth=20` |
| `rest_contracts.json` | `GET https://archive.prod.nado.xyz/v2/contracts`, whole |
| `rest_time.txt` | `GET https://gateway.prod.nado.xyz/v1/time` |

## Websocket, `wss://gateway.prod.nado.xyz/v1/subscribe`

Subscribe with `{"method":"subscribe","stream":{"type":"best_bid_offer","product_id":2},"id":1}`.

| file | stream |
| --- | --- |
| `ws_bbo.json` | `best_bid_offer`, BTC-PERP |
| `ws_trade.json` | `trade`, BTC-PERP |
| `ws_depth_first.json`, `ws_depth_next.json` | two consecutive `book_depth` pushes; the second's `last_max_timestamp` is the first's `max_timestamp` |
| `ws_funding.json` | `funding_rate`, BTC-PERP |
| `ws_subscribe_ack.json` | the answer to a subscribe |
| `ws_subscribe_refused.json` | the answer to a repeated subscribe |
