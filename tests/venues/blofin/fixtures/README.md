# blofin fixtures

Captured live on 2026-09-25 between 05:48 and 05:50 UTC from `openapi.blofin.com`,
no credentials. Socket frames come from one `wss://openapi.blofin.com/ws/public`
session and are written verbatim. REST tables are TRIMMED: whole rows were cut
out of the original text, never re-serialised, so every number keeps the
venue's spelling.

| file | source |
|---|---|
| `rest_instruments.json` | `GET /api/v1/market/instruments`, 499 rows trimmed to BTC-USDT, BTC-USDC, BTC-USD (inverse), 1000BONK-USDT, NVDA-USDT (stock), WTIOIL-USDT and ONE-USDT |
| `rest_funding_rate.json` | `GET /api/v1/market/funding-rate`, 499 rows trimmed to six: 8 h, 4 h and the two 1 h contracts |
| `rest_mark_price.json` | `GET /api/v1/market/mark-price`, trimmed to BTC-USDT and BTC-USD |
| `ws_books_snapshot.json` | `books` WTIOIL-USDT, the snapshot: 400 levels a side, `prevSeqId` "0" |
| `ws_books_update.json`, `ws_books_update_next.json` | the two deltas that followed it; the chain holds, and the first deletes the best bid with size "0" |
| `ws_trades.json` | `trades` BTC-USDT, one print |
| `ws_tickers.json`, `ws_tickers_inverse.json` | `tickers` BTC-USDT and BTC-USD; on the inverse one `vol24h` equals `volCurrency24h` |
| `ws_funding_rate.json` | `funding-rate` BTC-USDT, a rate with 21 fractional digits and no period |
| `ws_subscribe_ack.json` | the acknowledgement of one argument |
| `ws_error_invalid_request.json` | the refusal of a frame naming an unlisted instrument: code 60012, no `arg`, the frame echoed in `msg` |

## What these bytes settle

**The period moves.** In the funding table ONE-USDT states 1 hour; its
`funding-rate-history` shows 4-hour settlements until 2026-09-25 00:00 UTC and
hourly ones from 01:00. The table is re-read every minute for this reason.

**The socket's funding row has no period.** Only the REST table states
`fundingInterval` and `fundingIntervalUnit`.
