# okx fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Everything under "Websocket" and "REST" below was **captured live** from
production OKX on 2026-08-10. No credentials are involved — those are public
endpoints and the public websocket.

The private-side files under "Private (documented, not captured)" are the
exception, and the reason is stated there: placing an order needs credentials
this repository does not have.

## Websocket — `wss://ws.okx.com:8443/ws/v5/public`

One session, subscribing `bbo-tbt`, `trades`, `funding-rate` and `books` on
`BTC-USDT-SWAP`, read for 15 s.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `bbo-tbt` acknowledgement of that subscribe |
| `ws_bbo_tbt.json` | first `bbo-tbt` push |
| `ws_trades.json` | first `trades` push |
| `ws_funding_rate.json` | the `funding-rate` push that follows a subscribe |
| `ws_books_snapshot.json` | the `books` snapshot, all 400 levels a side, untrimmed |
| `ws_books_update.json`, `ws_books_update_next.json` | the next two `books` deltas |

The three `books` files are **consecutive frames off one socket**, so the
`prevSeqId` → `seqId` chain across them is real rather than constructed:
`snapshot.seqId == update.prevSeqId` and `update.seqId == update_next.prevSeqId`.
Each delta also carries `"checksum":0` — the field was deprecated on
2026-06-23 and is now pinned to zero, which is exactly why the parser does not
surface it and nothing validates against it.

`ws_error.json` came from a second session that subscribed to a channel that
does not exist; OKX answered `{"event":"error","code":"60018",...}`.

## REST — `https://www.okx.com`

| file | source |
|---|---|
| `rest_instruments.json` | `GET /api/v5/public/instruments?instType=SWAP` (443 rows) plus one row from `?instType=FUTURES`, trimmed to four |
| `rest_mark_price.json` | `GET /api/v5/public/mark-price?instType=SWAP&instId=BTC-USDT-SWAP` |
| `rest_funding_rate.json` | `GET /api/v5/public/funding-rate?instId=BTC-USDT-SWAP` |
| `rest_server_time.json` | `GET /api/v5/public/time` |
| `rest_error_bad_instrument.json` | the same mark-price call with `instId=NOPE-USDT-SWAP` — a real `51001` body |

`rest_instruments.json` is the only assembled file, and only in the sense that
rows were **selected**: each of the four is a verbatim row from a live
response, re-emitted inside the envelope the venue sent. It keeps

- `BTC-USDT-SWAP` and `ETH-USDT-SWAP` — `SWAP`/`live`, both loadable, and both
  carrying a `ctVal` that is not 1 (0.01 BTC and 0.1 ETH a contract), which is
  the whole point of not hardcoding contract size;
- `JP225-USDT-SWAP` — `state: "preopen"`, and every numeric field on it is the
  empty string. The perp loader must skip it, and it must skip it on `state`
  before it tries to parse a tick size, or it fails for the wrong reason;
- `BTC-USD-260814` — `instType: "FUTURES"`. Asking for perps and getting a
  dated contract back is a position bug that only surfaces at settlement.

A loader that accepts all four is wrong in a way that stays invisible for
months.

## Not captured

There is no `mark-price` websocket fixture. The channel exists and
`parse_mark_price` reads it, but OKX ships the identical object on
`GET /public/mark-price`, so `rest_mark_price.json` exercises the same parser
against real bytes. The same is true of `funding-rate`, where the REST and
websocket payloads were compared field-for-field and matched.


## Private (documented, not captured)

Nothing here was observed on a live account: order entry needs an API key, a
secret and a passphrase, and none exist for this repository. These files are
therefore the next best evidence — the response and push examples OKX
publishes in its own v5 documentation, copied byte-for-byte, plus two files
that are assembled and say so.

**Copied verbatim from the OKX v5 docs (2026-08 revision):**

| file | source |
|---|---|
| `ws_orders_fill.json` | the WS *Order channel* "Push Data Example" — a SPOT `BTC-USDT` sell that filled whole, `execType: "M"`, `tradeId` present |
| `ws_login_ok.json` | the WebSocket *Login* success response |
| `ws_login_failed.json` | the WebSocket *Login* failure response, code `60009` |
| `rest_order_ack.json` | `POST /api/v5/trade/order` success response |
| `rest_orders_pending.json` | `GET /api/v5/trade/orders-pending` response — one `state: "live"` post-only order, `accFillSz "0"`, and `fillTime ""` rather than absent |
| `rest_fills.json` | `GET /api/v5/trade/fills` response |
| `rest_positions.json` | `GET /api/v5/account/positions` response |

**Assembled, and only these two:**

| file | how it was built |
|---|---|
| `rest_order_reject.json` | the REST envelope shape OKX documents for a refused order (`code "1"`, reason in `data[].sCode`) carrying the per-order object verbatim from the docs' `51008` example, with `clOrdId` set to a ULID so the ack-to-order match has something to match on |
| `ws_orders_partial_expiry.json` | the `ws_orders_fill.json` object, re-valued on `BTC-USDT-SWAP` as a 5-contract IOC that filled 2 and was then killed: `state: "canceled"` with `cancelSource: "14"` **and** a `tradeId` in the same message |

That second file exists because it is the case a reader gets wrong. OKX sends
the last fill and the cancel that ended the order as ONE object, so an adapter
that maps state or fills but not both loses either the money or the terminal
event. Every number in it divides exactly by the `BTC-USDT-SWAP` tick (0.1)
and lot (0.01) the mapping tests load, which is the only thing about it that
is not copied.

`rest_positions.json` is a `MARGIN` row rather than a `SWAP` one because that
is the row OKX publishes. The parser test reads it as-is; the sign and lot
conversions are exercised in `map_test.cpp` against constructed rows, where a
short position can be written without inventing a live response.
