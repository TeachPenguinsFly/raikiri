# Ostium fixtures

Captured live from `builder.prod.bedrock.ostium.io` on 2026-10-06 around 15:20 UTC, without keys.

| file | source |
|---|---|
| `rest_pairs.json` | `GET /v1/pairs`, the 83 pairs with an on-chain id |
| `rest_status.json` | `GET /v1/status`, whose `time` is the clock probe |
| `rest_subgraph_pairs.json` | `POST /v1/subgraph/gn` with the adapter's rollover query: open interest and rollover per block, scaled by 1e18 |
| `rest_subgraph_orders.json` | `POST /v1/subgraph/gn` with the adapter's fills query for the hour before capture |
| `rest_subgraph_error.json` | the subgraph's answer to a query naming a field that does not exist |
| `ws_snapshot.json` | `wss://…/v1/prices/stream?pairs=BTC-USD,EUR-USD,NVDA-USD,US500-USD`, the frame on connect |
| `ws_tick.json` | the same stream, one BTC-USD tick |
| `ws_ack.json` | the answer to `{"type":"subscribe","pairs":["XAU-USD"]}` |
| `ws_error.json` | the answer to `{"type":"bogus"}` |
