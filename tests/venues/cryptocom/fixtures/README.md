# cryptocom fixtures

Captured live on 2026-09-24 from production crypto.com Exchange, public paths
only, no credentials.

| file | source |
|---|---|
| `rest_instruments.json` | `GET https://api.crypto.com/exchange/v1/public/get-instruments`, seven of 980 rows kept verbatim: `BTC_USD`, `BTC_USDT`, `CRO_BTC`, `BTCUSD-PERP`, `AAPLUSD-PERP`, `MOGUSD-PERP`, `BTCUSD-260925` |
| `rest_tickers.json` | `GET https://api.crypto.com/exchange/v1/public/get-tickers` on 2026-10-06, five of 989 rows kept verbatim, `BTC_USDT` and `BTCUSD-PERP` among them |
| `ws_index_btc.json` | `index.BTCUSD-INDEX` on the market socket, the first push after the subscribe answer, 2026-10-06 |
| `rest_error.json` | `GET /exchange/v1/public/get-book?instrument_name=NOPE_NOPE&depth=10` (HTTP 400) |
| `ws_*` | `wss://stream.crypto.com/exchange/v1/market`, one session subscribing `book.{i}.50` (SNAPSHOT_AND_UPDATE, 100 ms), `trade`, `ticker`, `funding`, `estimatedfunding`, `mark` on `BTCUSD-PERP` and `BTC_USD` |
| `ws_book_perp.json`, `ws_book_update_perp.json`, `ws_book_update_perp_next.json` | a snapshot and the two deltas after it, consecutive |
| `*_initial.json` | a subscription's first answer (request id, not -1) |
| `ws_error_unsupported.json` | the same session asking the book at 500 ms |
| `ws_error_max_subscriptions.json` | a socket holding 400 `ticker` channels asked for a 401st |
| `fuzz_*.json` | not captured: inputs a fuzzer found crashing the book parser |
