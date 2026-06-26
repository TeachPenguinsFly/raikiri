# Pacifica fixtures

Captured live from `api.pacifica.fi` and `wss://ws.pacifica.fi/ws` on
2026-09-26 around 08:46 UTC, without keys.

| file | source |
|---|---|
| `rest_info.json` | `GET /api/v1/info`, six rows kept verbatim: SOL, BTC, kBONK, CL and XAU (orderbook + RFQ), SOL-USDC (spot) |
| `ws_book.json` | `book` source, BTC at `agg_level` 1: ten levels a side, whole ladder |
| `ws_bbo.json` | `bbo` source, BTC |
| `ws_trades.json` | `trades` source, BTC: one taker across three levels |
| `ws_prices.json` | `prices` source, the six symbols above kept from the all-market array |
| `ws_subscribe.json`, `ws_pong.json` | the subscribe ack and the answer to `{"method":"ping"}` |
| `ws_error.json` | the answer to a `book` subscription for a symbol that does not exist |
