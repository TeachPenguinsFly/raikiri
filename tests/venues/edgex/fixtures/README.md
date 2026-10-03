# edgex fixtures

Captured live on 2026-09-25 between 05:44 and 05:46 UTC, no credentials. Perp
frames come from one `wss://edgex-quote-prod-v2.edgex.exchange/api/v1/public/ws`
session, spot frames from one `wss://spot-quote.edgex.exchange/api/v1/public/ws`
session. Frames are verbatim; where a fixture is trimmed, whole objects were
cut out of the original text by brace matching, so every number keeps the
venue's spelling.

| file | source |
|---|---|
| `rest_meta_perp.json` | `GET edgex-prod-v2.edgex.exchange/api/v2/public/meta/getMetaData`, trimmed to BTC (30000001), MRVL (30000077), KORU (30000089), 哈基米 (30000169) and USDJPY (30000174) and their coins; `multiChain` and `campaign` dropped |
| `rest_meta_spot.json` | `GET spot.edgex.exchange/api/v1/public/meta/getMetaData`, whole |
| `ws_depth_snapshot.json`, `ws_depth_changed.json`, `ws_depth_changed_next.json` | `depth.30000003.15` (SOL): the `Snapshot` and the two `changed` frames after it |
| `ws_trades_snapshot.json` | `trades.30000001`, the `Snapshot` backlog trimmed to 2 prints |
| `ws_trades_changed.json` | `trades.30000001`, one `changed` frame |
| `ws_ticker_all.json` | `ticker.all.1s`, one `changed` frame trimmed to the five catalogue contracts |
| `ws_funding_rate_all.json` | `fundingRate.all`, the `Snapshot` of the same session, trimmed the same way |
| `rest_latest_funding_rate.json` | `GET /api/v2/public/funding/getLatestFundingRate?contractId=<all 180>`, fetched as `ws_ticker_all.json` arrived, trimmed the same way |
| `ws_error_contract.json`, `ws_error_depth.json`, `ws_error_channel.json` | refusals of `depth.99999999.15`, `depth.30000001.50` and `nosuch.30000001` |
| `ws_ping.json`, `ws_subscribed.json` | the server's ping and a subscription ack |
| `ws_spot_depth_snapshot.json`, `ws_spot_trades_changed.json`, `ws_spot_ticker_all.json` | `depth.90000002.15`, `trades.90000002` and `ticker.all.1s` on the spot socket |
