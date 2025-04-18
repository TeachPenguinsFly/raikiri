# BingX fixtures

Every file here was captured from BingX production on 2026-08-10/11. Nothing is
hand-written and nothing is synthesised; where a file was trimmed, the trimming
is described below and the surviving rows are byte-for-byte what the venue sent.

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
| `ws_depth5.json` | `BTC-USDT@depth5@100ms` | full 5-level snapshot; `ts` sits beside `data`, no sequence number |
| `ws_mark_price.json` | `BTC-USDT@markPrice` | mark only — BingX streams no funding rate anywhere |
| `ws_subscribe_ack.json` | any | `code` 0, `dataType` empty, echoing the `id` that asked |
| `ws_error.json` | `NOSUCH-USDT@bookTicker` | code 80015, unknown symbol |
| `ws_error_data_type.json` | `BTC-USDT@fundingRate` | code 80015 again, this time "dataType not support" — the probe that established funding has no channel |
| `ws_depth5.frame.gz` | — | the raw gzip frame that inflates to `ws_depth5.json`; its deflate block is dynamic-Huffman, so it exercises real decompression |
| `ws_ping.frame.gz` | — | the raw gzip frame carrying the four bytes `Ping`, stored uncompressed by the venue |

## REST (`https://open-api.bingx.com`)

| file | request |
| --- | --- |
| `rest_contracts.json` | `GET /openApi/swap/v2/quote/contracts`, trimmed to 4 rows |
| `rest_premium_index.json` | `GET /openApi/swap/v2/quote/premiumIndex?symbol=BTC-USDT` |
| `rest_server_time.json` | `GET /openApi/swap/v2/server/time` |
| `rest_error_bad_symbol.json` | `GET /openApi/swap/v2/quote/depth?symbol=NOSUCH-USDT&limit=5` → 109425 |
| `rest_error_not_found.json` | `GET /openApi/swap/v2/quote/nope` → 100400, the failure shape that drops `data` and adds `timestamp` |

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
