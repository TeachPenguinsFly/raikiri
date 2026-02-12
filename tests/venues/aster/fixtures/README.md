# aster fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Captured live on 2026-09-20 from production Aster USD-margined perpetuals. No
credentials: every path below is public.

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.asterdex.com/stream?streams=btcusdt@bookTicker` |
| `ws_agg_trade.json` | `btcusdt@aggTrade` |
| `ws_mark_price.json` | `btcusdt@markPrice@1s` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two CONSECUTIVE frames so the `u`→`pu` chain is testable — captured as one session, and the chain held (`u` 554245272553 = next `pu`) |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed from 602 symbols to three: one plain USDT perp, one USD1-quoted, one not yet trading |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_funding_info.json` | `GET /fapi/v1/fundingInfo`, trimmed to three rows |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_ticker_24h.json` | `GET /fapi/v1/ticker/24hr?symbol=BTCUSDT` |
| `rest_server_time.json` | `GET /fapi/v1/time` |

## What these bytes are for

This API is wire-compatible with a much larger venue's, and the parsers were
forked from that adapter. THAT IS WHY THESE FIXTURES EXIST RATHER THAN WHY
THEY DO NOT: a fork drifts silently. The catalogue already differs in a way
that matters — 11 of 602 rows are quoted in `USD1` and 2 in `U`, so a mapper
that assumed USDT would drop exactly the thin contracts a funding board is
looking for.

The request WEIGHTS differ too, and are asserted in the test file rather than
captured here: `premiumIndex` and `fundingInfo` cost 10 on this venue against
1 on its origin, measured off the `x-mbx-used-weight-1m` header.
