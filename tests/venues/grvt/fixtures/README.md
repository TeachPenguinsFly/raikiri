# grvt fixtures

Captured live on 2026-09-24 from production GRVT, `market-data.grvt.io`. No
credentials: every path below is public.

| file | source |
|---|---|
| `rest_all_instruments.json` | `POST /full/v1/all_instruments {}`, trimmed from 194 rows to four, each row verbatim: `BTC_USDT_Perp` (8 h funding), `ASTER_USDT_Perp` (4 h), `ADA_USDT_Perp` (a 0.375 % cap) and `AI16Z_USDT_Perp` (DELISTED) |
| `rest_error.json` | `POST /full/v1/all_instruments` with a body that is not JSON — HTTP 400 |
| `ws_book_d_ack.json`, `ws_book_d_snapshot.json`, `ws_book_d_delta.json`, `ws_book_d_delta_next.json` | `wss://market-data.grvt.io/ws/full`, `v1.book.d` selector `ETH_USDT_Perp@500`: the ack, the snapshot and the first two deltas of ONE subscription, in order — the ack's `latest_sequence_number` 10971 is the first delta's `prev_sequence_number`, and that delta's 10972 the next one's |
| `ws_mini_d_snapshot.json`, `ws_mini_d_delta.json` | `v1.mini.d` selector `ETH_USDT_Perp@0`, the snapshot and the first delta after it, same session |
| `ws_trade_snapshot.json`, `ws_trade.json` | `v1.trade` selector `ETH_USDT_Perp@50`: one of the 50 replayed prints (sequence 0) and the first live one |
| `ws_ticker_s.json` | `v1.ticker.s` selector `ETH_USDT_Perp@5000` |
| `ws_error.json` | `v1.book.d` asked for `NOPE_USDT_Perp@500` |

Captured with a plain websocket client sending, per stream,

    {"jsonrpc":"2.0","method":"subscribe","params":{"stream":"v1.book.d","selectors":["ETH_USDT_Perp@500"]},"id":1}

and writing the first frame of each shape to disk unchanged.

## The funding interval

Stated per contract as `funding_interval_hours` — on every catalogue row and on
every ticker frame — so nothing here is a constant. It was checked against the
venue's own settlement history (`POST /full/v1/funding`, the last eight rows
each) the same day: BTC, AAVE, AAPL and AAOI settled 8 h apart and ASTER, AVNT,
AXS, BARD, BASED and BERA 4 h apart, each matching the interval its row states.
Of the 184 active contracts, 124 settle every eight hours and 60 every four.

Every funding figure is in PERCENT of one interval: `"0.01"` on an 8 h contract
is one basis point per eight hours. The four-hour contracts sit at `"0.005"` when
quiet, half the eight-hour default, which is what says the rate is not
normalised to eight hours despite the `funding_rate_8h_curr` field name.
