# edgex fixtures

Captured live on 2026-09-25 between 05:44 and 05:46 UTC, no credentials. Perp
frames come from one `wss://edgex-quote-prod-v2.edgex.exchange/api/v1/public/ws`
session, spot frames from one `wss://spot-quote.edgex.exchange/api/v1/public/ws`
session. Frames are written verbatim; where a fixture is trimmed, whole objects
were cut out of the original text by brace matching, so every number keeps the
venue's spelling.

| file | source |
|---|---|
| `rest_meta_perp.json` | `GET edgex-prod-v2.edgex.exchange/api/v2/public/meta/getMetaData`, trimmed to BTC (30000001), MRVL (30000077), KORU (30000089, a stock), 哈基米 (30000169, base coin HAJIMI) and USDJPY (30000174, FX) and their coins; `multiChain` and `campaign` dropped |
| `rest_meta_spot.json` | `GET spot.edgex.exchange/api/v1/public/meta/getMetaData`, whole: four pairs |
| `ws_depth_snapshot.json`, `ws_depth_changed.json`, `ws_depth_changed_next.json` | `depth.30000003.15` (SOL): the `Snapshot` and the two `changed` frames after it, chained by `startVersion` = previous `endVersion` |
| `ws_trades_snapshot.json` | `trades.30000001`, the `Snapshot` backlog trimmed from 30 prints to 2 |
| `ws_trades_changed.json` | `trades.30000001`, a `changed` frame with two prints of one taker order |
| `ws_ticker_all.json` | `ticker.all.1s`, the `changed` frame received with the REST call below, trimmed from 180 rows to the five catalogue contracts |
| `ws_funding_rate_all.json` | `fundingRate.all`, the `Snapshot` of the same session, trimmed the same way |
| `rest_latest_funding_rate.json` | `GET /api/v2/public/funding/getLatestFundingRate?contractId=<all 180>`, fetched on the ticker frame above, trimmed the same way |
| `ws_error_contract.json`, `ws_error_depth.json`, `ws_error_channel.json` | refusals of `depth.99999999.15`, `depth.30000001.50` and `nosuch.30000001` |
| `ws_ping.json`, `ws_subscribed.json` | the server's ping and a subscription ack |
| `ws_spot_depth_snapshot.json`, `ws_spot_trades_changed.json`, `ws_spot_ticker_all.json` | `depth.90000002.15`, `trades.90000002` and `ticker.all.1s` on the spot socket |

## What these bytes settle

`fundingRate` on `ticker.*` is the forecast for `nextFundingTime`; on
`fundingRate.*` and REST `getLatestFundingRate` it is the last settled rate. In
the same second, 135 of 180 contracts had settled ≠ forecast, and on every one of
them the ticker carried the forecast (MRVL: settled `-0.0004477`, forecast and
ticker `-0.0004145`). `predictedFundingRate` was `0.00005000` on all 180: the
interest leg, not a prediction.
