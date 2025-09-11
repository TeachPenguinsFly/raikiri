# BingX fixtures

Every PUBLIC file here was captured from BingX production. Nothing in that set
is hand-written and nothing is synthesised; where a file was trimmed, the
trimming is described below and the surviving rows are byte-for-byte what the
venue sent.

The PRIVATE files are different and the difference is stated per file below.
There is no API key for this account, so no authenticated response could be
captured. Those fixtures are BingX's own published example payloads — the ones
its documentation ships as the response of each endpoint — with our own client
order id substituted where the published example leaves the field empty, and
with a symbol swapped to one this repo's catalogue fixtures carry. They are
schema fixtures, not evidence: they pin the field names, the nesting and the
number encodings, and they cannot vouch for a value BingX has never sent us.

Two things about provenance are worth stating plainly, because both are easy to
get wrong when refreshing these.

**The websocket fixtures were inflated.** BingX sends every frame gzip-compressed
under a binary opcode, so what came off the socket was binary. It was captured
with `websocat --base64` — websocat's default stdout path mangles non-UTF-8
bytes, and the first attempt at this silently corrupted every frame that used a
real Huffman block — and each message was then inflated with
`python3 -c "import zlib; zlib.decompress(raw, 31)"`. The `.json` files are the
inflated text; the `.frame.gz` files are the raw bytes, kept so the Inflater can
be tested against what the venue actually transmits.

**The REST fixtures are raw responses**, saved straight from `curl` against
`https://open-api.bingx.com`. Only `rest_contracts.json` was edited: the live
response carries 1021 rows and this keeps 4 (see below).

## Websocket (`wss://open-api-swap.bingx.com/swap-market`)

| file | subscription | notes |
| --- | --- | --- |
| `ws_book_ticker.json` | `BTC-USDT@bookTicker` | `data` is an object; `T` is the book change, `E` the send |
| `ws_trade.json` | `BTC-USDT@trade` | `data` is an array; `m` = buyer was the maker; no trade id |
| `ws_depth100.json` | `BTC-USDT@depth100@100ms` | recaptured 2026-08-17: full 100-level snapshot; `ts` sits beside `data`, no sequence number. 100 is the deepest tier — `depth200` drew 80015 "dataType not support" |
| `ws_mark_price.json` | `BTC-USDT@markPrice` | mark only — BingX streams no funding rate anywhere |
| `ws_ticker.json` | `BTC-USDT@ticker` | captured 2026-09-04: the 24-hour window. `v` is base and `q` quote — 1723468750.41 / 21355.7400 = 80704, inside the same frame's 78949.9-82264.7 band. Both QUOTED, the perpetual tree's habit |
| `ws_spot_ticker.json` | `BTC-USDT@ticker` on the SPOT socket | the same channel and the same two columns sent as bare JSON numbers, captured in the same minute. The envelope also stamps `timestamp` where the swap stamps nothing |
| `ws_subscribe_ack.json` | any | `code` 0, `dataType` empty, echoing the `id` that asked |
| `ws_error.json` | `NOSUCH-USDT@bookTicker` | code 80015, unknown symbol |
| `ws_error_data_type.json` | `BTC-USDT@fundingRate` | code 80015 again, this time "dataType not support" — the probe that established funding has no channel |
| `ws_depth100.frame.gz` | — | the raw gzip frame that inflates to `ws_depth100.json`; its deflate block is dynamic-Huffman, so it exercises real decompression |
| `ws_ping.frame.gz` | — | the raw gzip frame carrying the four bytes `Ping`, stored uncompressed by the venue |

## REST (`https://open-api.bingx.com`)

| file | request |
| --- | --- |
| `rest_contracts.json` | `GET /openApi/swap/v2/quote/contracts`, trimmed to 4 rows |
| `rest_premium_index.json` | `GET /openApi/swap/v2/quote/premiumIndex?symbol=BTC-USDT` |
| `rest_server_time.json` | `GET /openApi/swap/v2/server/time` |
| `rest_error_bad_symbol.json` | `GET /openApi/swap/v2/quote/depth?symbol=NOSUCH-USDT&limit=5` → 109425 |
| `rest_error_not_found.json` | `GET /openApi/swap/v2/quote/nope` → 100400, the failure shape that drops `data` and adds `timestamp` |
| `rest_error_bad_key.json` | `POST /openApi/swap/v2/trade/order` with no key, captured 2026-08-26 → **HTTP 200** carrying 100413. This is the proof behind reading the envelope and never the status |
| `rest_spot_symbols.json` | `GET /openApi/spot/v1/common/symbols`, captured 2026-08-26, trimmed from 2258 rows to 5 (see below) |

### The five spot rows, and why each one is there

- `BTC-USDT` — the ordinary case: `tickSize` 0.01, `stepSize` 0.000001, and the
  row every spot mapping test measures against.
- `ETH-USDT` — a second online row so "the mapper accepted one" cannot pass by
  accident.
- `APEPE-USDT` — online and usable everywhere except here: its `tickSize` is
  `1e-10`, finer than FixedE9 holds, so the mapper MUST refuse it rather than
  round it up to a price grid BingX does not use. It also proves the wire
  really does carry scientific notation, which `parse_decimal_e9` cannot read.
- `ANSEM-USDT` — `status` 1 but `apiStateBuy`/`apiStateSell` false: listed and
  priced for the web, closed to API orders. 36 of the 705 online pairs are like
  this, and nothing but that second gate keeps them out.
- `M3M3-USDT` — `status` 0, the plain offline case.

### The four contract rows, and why each one is there

- `BTC-USDT` — the ordinary case, and the one every mapping test measures
  against: `pricePrecision` 1, `quantityPrecision` 4.
- `ETH-USDT` — a second online row so "the mapper accepted one" cannot pass by
  accident.
- `SOL-USDT` — online, and the row where `size` ("1") contradicts
  `quantityPrecision` (2). Live SOL quantities carry two decimals, so the
  precision is right and `size` is not a step. 4 of 1021 rows disagree this way.
- `NCCOCOFFEE2USD-USDT` — `status` 25, not 1: the row the mapper MUST refuse. It
  ships complete, usable precisions, so nothing but the status check keeps it
  out. It also carries `tradeMinQuantity` as `7e-05`, the scientific notation
  that `parse_decimal_e9` refuses.

## Private (constructed from BingX's published examples — no key exists)

| file | shape it pins |
| --- | --- |
| `rest_open_orders_perp.json` | `GET /openApi/swap/v2/trade/openOrders` → `data.orders[]`. BingX's own example, with `clientOrderId` filled in (its example leaves it empty) **lowercased**, which is what the venue does to the field, and a second row added in `PARTIALLY_FILLED` so the leaves arithmetic is measured and not assumed |
| `rest_open_orders_spot.json` | `GET /openApi/spot/v1/trade/openOrders` → the same `data.orders[]` with spot's field set: `clientOrderID`, `cummulativeQuoteQty`, `status` `PENDING` |
| `rest_order_ack_perp.json` | `POST /openApi/swap/v2/trade/order` → `data.`**`order`**`{}`, one level deeper than spot |
| `rest_order_ack_spot.json` | `POST /openApi/spot/v1/trade/order` → `data{}`, flat |
| `rest_positions.json` | `GET /openApi/swap/v2/user/positions` → a bare `data[]`, `positionAmt` UNSIGNED with `positionSide` carrying the sign |
| `rest_listen_key.json` | `POST /openApi/user/auth/userDataStream` → the bare `{"listenKey":…}` its skill reference shows |
| `rest_listen_key_wrapped.json` | the same answer in the usual envelope, which its API reference shows instead. Both are read, because which one arrives decides whether the user stream connects at all |
| `rest_position_mode.json` | `GET /openApi/swap/v1/positionSide/dual` → `dualSidePosition` as the STRING `"true"`, not a JSON bool |
| `ws_order_update_perp_new.json` | `ORDER_TRADE_UPDATE` with `x` `NEW`; every number QUOTED, which is the perpetual tree's habit |
| `ws_order_update_perp_fill.json` | the same order with `x` `TRADE`: `l`/`L` state the fill's own size and price, `z` the cumulative, `n` a negative fee. No trade id — the perpetual push mints none |
| `ws_order_update_perp_cancel.json` | `x` `CANCELED` with `X` spelled `CANCELLED`, the double-L variant BingX's own cancel answer uses |
| `ws_order_update_spot_fill.json` | `spot.executionReport` wrapped in `{code,dataType,data}`, every number BARE, a trade id in `t`, a maker flag in `m`, and NO client order id — the reason the adapter keeps a memo |
| `ws_listen_key_expired.json` | BingX's own published `listenKeyExpired` push, verbatim |
