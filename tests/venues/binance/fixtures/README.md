# binance fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Captured live from production binance USD-M futures (2025-04, no credentials
required — all of these are public endpoints):

| file | source |
|---|---|
| `ws_book_ticker.json` | `wss://fstream.binance.com/stream?streams=btcusdt@bookTicker` |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `btcusdt@depth@100ms`, two consecutive frames so the `pu`→`u` chain is testable |
| `rest_exchange_info.json` | `GET /fapi/v1/exchangeInfo`, trimmed from 858 symbols to four |
| `rest_premium_index.json` | `GET /fapi/v1/premiumIndex?symbol=BTCUSDT` |
| `rest_depth_snapshot.json` | `GET /fapi/v1/depth?symbol=BTCUSDT&limit=5` |
| `rest_depth_seed.json`, `ws_depth_straddle.json`, `ws_depth_straddle_next.json` | one capture (2026-08), `limit=100` — see below |
| `rest_server_time.json` | `GET /fapi/v1/time` |
| `rest_income.json` | `GET /fapi/v1/income` (2026-09-16), one COMMISSION row and one FUNDING_FEE row — SHAPE captured live, figures replaced: what matters here is that `tradeId` is a STRING on both while `tranId` beside it is a number |
| `rest_funding_info.json` | `GET /fapi/v1/fundingInfo` (2026-09), trimmed from 782 rows to one of each interval binance publishes — 8h, 4h and 1h |
| `rest_error_bad_symbol.json` | `GET /fapi/v1/ticker/bookTicker?symbol=NOSUCHSYM` |
| `rest_error_bad_apikey.json` | `GET /fapi/v1/order?symbol=NOPE` |

Assembled, not captured:

| file | how |
|---|---|
| `ws_agg_trade.json` | numbers are a real trade from `GET /fapi/v1/aggTrades?limit=1`; the `e`/`E`/`s` envelope is binance's documented `aggTrade` wrapper |
| `ws_mark_price.json` | numbers are a real `premiumIndex` response; the envelope is the documented `markPriceUpdate` wrapper |
| `rest_account.json` | `GET /fapi/v2/account` is signed and no recorded capture exists; the shape and every field name are binance's documented ones. Twenty asset rows — two with money in them and eighteen binance merely accepts as collateral — plus the two fields that made this the endpoint the wallet is read from: `multiAssetsMargin`, which says how the per-asset `availableBalance` is to be read, and the `positions` array the reader walks past. The USDT row's 11982.40 free against 12425.26 held is the margin an open position has already committed |

The assembled three are honest about their field values but were not observed
on the socket, so they prove the parser reads the documented shape — not that
the shape is what production sends. Replace them with captures when a recorder
exists.

## The seed triple

`rest_depth_seed.json`, `ws_depth_straddle.json` and `ws_depth_straddle_next.json`
are one capture, not three files that happen to be about depth: the socket was
recorded first, `GET /fapi/v1/depth?symbol=BTCUSDT&limit=100` was issued two
seconds in, and the two frames kept are the one whose `[U, u]` contains that
snapshot's `lastUpdateId` and the one immediately after it. A seed and a stream
taken minutes apart replay into a *crossed* book, which is exactly what the
book tests exist to catch, so provenance here is the test.

Two facts the capture pins down, both measured rather than read off the docs:

- The chain is `pu == the previous frame's u`. Over 105 consecutive frames the
  spot rule (`U == previous u + 1`) held **zero** times.
- Ids are not dense per symbol — consecutive frames left holes of roughly a
  hundred — so a snapshot can land where no frame straddles it. That seed is
  unusable, and the book discards it instead of splicing two different books.

`rest_exchange_info.json` keeps four symbols on purpose: BTCUSDT and ETHUSDT
(`PERPETUAL`/`TRADING`, both loadable), XAUUSDT (`TRADIFI_PERPETUAL` — a
contract type the perp loader must skip) and OMGUSDT (`SETTLING` — a status it
must skip). A loader that accepts all four is wrong in a way that only shows
up months later, on a delisting.
