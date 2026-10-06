# Ondo Perps fixtures

Captured live from `api.ondoperps.xyz` and `wss://api.ondoperps.xyz/ws` on 2026-10-06 between 15:10
and 15:12 UTC, without keys. The REST answers and the all-market frames keep eleven markets: BTC,
ETH, TSLA, NVDA, SPY, XAU, WTI, US500, USDJPY, and the disabled EURUSD and LIT.

| file | source |
|---|---|
| `rest_markets.json` | `GET /v1/markets`, the eleven perps and one spot row |
| `rest_open_interest.json` | `GET /v1/perps/open_interest` |
| `rest_volume.json` | `GET /v1/perps/volume`, 24 h base and quote |
| `rest_depth_time.json` | `GET /v1/perps/depth?market=BTC-USD.P`, whose `time` is the clock probe |
| `ws_top.json` | `topOfBooksPerps` for BTC-USD.P |
| `ws_depth.json` | `depthBooksPerps` for BTC-USD.P with `limit` 20 |
| `ws_trades.json` | `tradesPerps` for BTC-USD.P |
| `ws_marks.json` | `markPricesPerps` for every market, the kept rows |
| `ws_funding.json` | `fundingRatesPerps` for every market, the kept rows with their minute premiums |
| `ws_subscribed.json` | a subscribe ack |
| `ws_error.json` | the answer to a subscribe naming a channel that does not exist |
| `ws_pong.json` | the answer to `{"op":"ping"}` |
