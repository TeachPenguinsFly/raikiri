# lighter fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Captured live on 2026-09-24 from mainnet (`mainnet.zklighter.elliot.ai`), no
credentials: every path below is public. Frames were recorded off one
`wss://mainnet.zklighter.elliot.ai/stream` session per group and written out
verbatim; where a fixture is TRIMMED, rows were cut out of the original text by
brace matching rather than re-serialised, so every number keeps the spelling
the venue gave it — the daily volumes are JSON floats, and a round trip through
any JSON library would respell some of them.

| file | source |
|---|---|
| `rest_order_book_details.json` | `GET /api/v1/orderBookDetails`, trimmed from 235 perps and 11 spot pairs to BTC (1), LIT (120), ARC (132, funding clamp 20) and DUSK (125, inactive); ETH/USDC (2048), rhSPY/USDC (2057, token multiplier ≠ 1) and AZTEC/USDC (2055, inactive) |
| `ws_order_book_snapshot.json` | `order_book/2054` (LDO/USDC spot), the `subscribed/order_book` frame — the whole book, 24 bids and 17 asks |
| `ws_order_book_update.json`, `ws_order_book_update_next.json` | the two `update/order_book` frames that followed it in the SAME session; the chain held (`begin_nonce` of each = the previous `nonce`), and the second deletes the best bid with a size of `0.0` |
| `ws_trade_update.json` | `trade/1`, one `update/trade` frame carrying three prints of one match |
| `ws_trade_snapshot.json` | `trade/1`, the `subscribed/trade` backlog, trimmed from 50 trades and 50 liquidations to one of each |
| `ws_market_stats.json` | `market_stats/1`, one `update/market_stats` frame |
| `ws_market_stats_all.json` | `market_stats/all`, the `subscribed/market_stats` frame trimmed from 235 rows to BTC, LIT (negative rate) and DUSK (inactive, last settled months earlier) |
| `ws_spot_market_stats.json` | `spot_market_stats/2048`, one `update/spot_market_stats` frame |
| `ws_error_rate_limit.json` | the refusal sent for every subscription past 200 a minute from one IP, as printed by the limit probe |
| `ws_error_too_many_subscriptions.json` | the refusal sent for the 501st subscription on one socket, as printed by the limit probe |

## What these bytes settle

**Whose funding rate this is.** `current_funding_rate` on `market_stats` is
Lighter's own, in percent per hour. `GET /api/v1/funding-rates` — never read by
the adapter — is a comparison table carrying rows for lighter, binance, bybit
and hyperliquid under Lighter's market ids; its lighter rows equalled this
field times eight over one hundred for 210 of 210 markets on the capture day,
and matched the binance row for 1 of 180 markets that had one.

**The funding period.** Not in any fixture, because no frame states it:
`/api/v1/fundings?resolution=1h` for markets 0, 1 and 188 over the 48 hours to
2026-09-24 20:00Z returned 48 settlements each, 47 gaps of exactly 3600 s.
That is `k_funding_interval_s`.
