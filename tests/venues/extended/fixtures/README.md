# Extended fixtures

Captured live from `api.starknet.extended.exchange` on 2026-09-25 around
05:44 UTC, without keys, with `User-Agent: raikiri/0.1` (an empty one is
refused 403).

| file | source |
|---|---|
| `rest_markets.json` | `GET /api/v1/info/markets`, ten rows kept verbatim: BTC, ADA (book), DASH, kFLOKI, 1000PEPE, TSLA_24_5 (RFQ), SHOP (RFQ, off hours), NOW_24_5 (DELISTED with `active:true`), GILD (PRELISTED), BTCSPOT (spot) |
| `ws_book_snapshot.json`, `ws_book_delta.json` | `/stream.extended.exchange/v1/orderbooks`, ADA-USD's snapshot and its next delta |
| `ws_book_delisted.json` | the same socket, a snapshot for a market the catalogue delisted |
| `ws_top.json` | `/stream.extended.exchange/v1/orderbooks?depth=1`, ADA-USD |
| `ws_trades_backlog.json` | `/stream.extended.exchange/v1/publicTrades`, the first frame: fifty prints older than the connection |
| `ws_trades.json` | the same socket, the second frame |
| `ws_funding.json`, `ws_funding_ghost.json` | `/stream.extended.exchange/v1/funding`, BTC-USD and MOG-USD (not in the catalogue) |
| `ws_mark.json` | `/stream.extended.exchange/v1/prices/mark`, DASH-USD; `data.ts` is 0 on every frame |
