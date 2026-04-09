# backpack fixtures

Captured live on 2026-09-24 from production Backpack Exchange, spot and
perpetuals. No credentials: every path below is public. The websocket frames
are byte-for-byte what `wss://ws.backpack.exchange` sent to a bare
`{"method":"SUBSCRIBE","params":[...]}`; the REST bodies are what
`https://api.backpack.exchange` answered, trimmed where noted.

| file | source |
|---|---|
| `ws_book_ticker.json` | `bookTicker.SOL_USDC` |
| `ws_trade.json` | `trade.SOL_USDC_PERP` |
| `ws_mark_price_perp.json` | `markPrice.SOL_USDC_PERP` — carries `f` (rate) and `n` (next funding, ms) |
| `ws_mark_price_spot.json` | `markPrice.SOL_USDC` — the same stream on a spot pair: a mark and no rate |
| `ws_depth.json`, `ws_depth_next.json` | `depth.200ms.SOL_USDC_PERP`, two CONSECUTIVE frames of one session, so the successor chain is testable: `u` 6684294071, next `U` 6684294072 |
| `rest_markets.json` | `GET /api/v1/markets`, trimmed from 190 rows to five: SOL spot, SOL perp, a stock perp (`NVDA.US_USDC_PERP`), a Closed spot pair (`HNT_USDC`) and a Closed perp (`IP_USDC_PERP`) |
| `rest_depth.json` | `GET /api/v1/depth?symbol=SOL_USDC_PERP&limit=5` |
| `rest_tickers.json` | `GET /api/v1/tickers`, trimmed to the two SOL rows |
| `rest_error.json` | `GET /api/v1/depth?symbol=NOPE_USDC`, the 400 body |

## Two clocks

Every stream timestamp (`E`, `T`) is in MICROseconds. The next funding time on
the same markPrice frame (`n`) is in MILLIseconds, as are the REST side's
stamps except the depth snapshot's `timestamp`, which is micro again. The
tests pin both readings, because either one mistaken for the other is a date
that is wrong by a factor of a thousand and still parses.
