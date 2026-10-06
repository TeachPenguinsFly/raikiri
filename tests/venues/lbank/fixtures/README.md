# LBank fixtures

Captured live on 2026-10-02 around 18:03-18:10 UTC, without keys, with the
user agent `raikiri/0.1` (Cloudflare refuses library defaults). Rows were cut
from the full answers and are otherwise verbatim.

| file | source |
|---|---|
| `rest_instrument.json` | `GET https://lbkperp.lbank.com/cfd/openApi/v1/pub/instrument?productGroup=SwapU`: BTCUSDT, ETHUSDT, HOODUSDT and CEGUSDT (`needSuspend` 1) |
| `rest_market_data.json` | `GET .../pub/marketData?productGroup=SwapU`: HOOD, BTC, ETH and 10TAUSDT, which has no `fundingRate` |
| `ws_book_snapshot.json` | `wss://lbkperpws.lbank.com/ws`, topic 25, filter `Exchange_BTCUSDT_0.1`: the first frame, three levels a side kept |
| `ws_book_delta.json` | the first diff after it: BTC and ETH rows at their own tick and at coarser `Tick` groupings |
| `ws_trade.json` | topic 2, filter `Exchange_BTCUSDT` |
| `ws_overview.json` | topic 7, filter `Exchange_BTCUSDT` |
| `ws_ack.json`, `ws_refusal.json` | `RecvTopicAction` for an accepted subscription and for `Exchange_NOPEUSDT_0.1` |
| `ws_resume_note.json` | the `PublishFlow` line that follows a subscription with `ResumeNo` -1 |
| `rest_accuracy.json` | `GET https://api.lbkex.com/v2/accuracy.do`: btc_usdt, eth_usdt and btc3l_usdt, which has no ticker |
| `rest_ticker_24hr.json` | `GET https://api.lbkex.com/v2/ticker/24hr.do?symbol=all`: btc_usdt and eth_usdt; the catalogue reads its symbols and, since 2026-10-06, the spot gateway polls it once a minute for the 24h volume of every pair (1049 rows on that date) |
| `spot_ws_depth.json`, `spot_ws_trade.json`, `spot_ws_tick.json` | `wss://api.lbank.info/ws/V2/`, `depth` (10), `trade` and `tick` for btc_usdt |
| `spot_ws_error.json` | the answer to a `depth` subscription for `nope_usdt` |
| `spot_ws_pong.json` | the answer to `{"action":"ping","ping":"x1"}` |

Added on 2026-10-03 between 15:13 and 15:20 UTC with the same user agent:

| file | source |
|---|---|
| `ws_book_batch_first.json`, `ws_book_batch_second.json` | topic 25 on one socket carrying twelve books: two frames of one batch under bNo 219701801916, labelled QNTUSDT and BTCUSDT, cut to the SUSHIUSDT and ACHUSDT rows that only one of them carries |
| `ws_book_replay.json` | the first diff on that socket's HYPEUSDT subscription, a replay older than the batches already seen, cut to HYPEUSDT rows |
| `rest_market_order.json` | `GET https://lbkperp.lbank.com/cfd/openApi/v1/pub/marketOrder?symbol=ETHUSDT&depth=5` |
