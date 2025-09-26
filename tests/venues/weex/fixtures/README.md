# WEEX fixtures

Everything here was captured from **production WEEX on 2026-09-07**. Nothing is
hand-written or synthesised. Two files are edited captures and say so; the rest
are verbatim response bodies and websocket frames, reformatted only by being
written as one line.

WEEX runs two exchanges under one brand and every file below is the
USDT-margined **contract** tree. Spot lives on `api-spot.weex.com` /
`ws-spot.weex.com` and this adapter never reaches it, so nothing here is spot.

- REST: `https://api-contract.weex.com`, plain `curl`.
- WS: `wss://ws-contract.weex.com/v3/ws/public`, subscribed with
  `{"method":"SUBSCRIBE","params":[...],"id":1}` and read with a short raw-socket
  script. `websocat` is not enough for this venue: `@depth200` sends frames past
  the fragmentation boundary, and WEEX's keepalive is an application-level
  `{"event":"ping"}` a pipe cannot answer.

Frames arrive as **uncompressed UTF-8 JSON**. No binary-frame support is needed
anywhere in this adapter.

## REST

| file | provenance |
| --- | --- |
| `rest_error_param_empty.json` | `GET /capi/v3/market/depth` with no parameters at all: `{"code":-1141,...}` at **HTTP 400**. |
| `rest_error_invalid_param.json` | `.../depth?symbol=BTCUSDT&limit=9999`: `{"code":-1142,...}` at **HTTP 400**. |
| `rest_error_not_found.json` | `GET /capi/v3/market/nope`. A path WEEX does not route answers a **different shape entirely** — `{"timestamp","status","error","path"}`, no `code` — at HTTP 404. It reads to the error parser as a success payload, which is why the caller checks the status too. |
| `rest_exchange_info.json` | **EDITED**: `GET /capi/v3/market/exchangeInfo` returned 1022 rows in 685 KiB. Eight rows were sliced out by brace matching (never re-serialised) and the envelope rebuilt as `{"symbols":[…]}`. Its `assets` and `rateLimits` arrays were dropped; no parser reads them. |
| `rest_premium_index.json` | **EDITED**: `GET /capi/v3/market/premiumIndex` returned 1022 rows in 229 KiB. Four rows were sliced out the same way and the array rebuilt. |

The eight catalogue rows were chosen so each isolates exactly one mapper
decision — a row refused for two reasons proves nothing about either:

| symbol | outcome | why |
| --- | --- | --- |
| `BTCUSDT` | accepted | `pricePrecision` 1 → tick 0.1, `contractVal` 0.0001 → lot, `delivery` of 3 → 28 800 s |
| `DOGEUSDT` | accepted | `quantityPrecision` is **−2**; `contractVal` 100 is the size step, and reading the precision as if it were positive would be four orders of magnitude out |
| `EURUSDT` | accepted | `TRADIFI_PERPETUAL`, and a `delivery` of 6 → 14 400 s |
| `ONGUSDT` | accepted | `delivery` of 24 → 3 600 s, the only hourly tier live (4 of 1022 rows) |
| `THEUSDT` | accepted | tick 1e-5, so its own premiumIndex `indexPrice` does **not** divide by it |
| `USDCUSDT` | accepted | needed to resolve the row that funds at exactly zero |
| `GER40USDT` | accepted | needed to resolve the row whose rate has eleven fractional digits |
| `QUBICUSDT` | **refused** | `pricePrecision` 10 → a 1e-10 tick, finer than `FixedE9` holds. 2 of the 1022 rows are like this |

There is **no status row**, because there is no status field: WEEX states none
on a contract, and every listed row is tradable. There is also no minimum
notional — `minOrderSize` is a base-asset quantity and equalled the size step on
1020 of the 1022 rows — so the instrument's `min_notional` stays UNDEF.

`contractVal` equalled 10^-`quantityPrecision` on **1022 of 1022 rows**, which
is why the decimal is read and the precision is not. The same body writes that
number as a JSON integer on 573 rows and as a float on 449.

The four premiumIndex rows:

| symbol | why |
| --- | --- |
| `BTCUSDT` | an ordinary row: rate, forecast, a mark that divides by the tick, `collectCycle` 480 |
| `USDCUSDT` | funds at **exactly zero** (`"0.000"`). A contract funding at zero and one quoting no funding are different facts and both have to survive the mapper |
| `GER40USDT` | `lastFundingRate` is `"0.00000001988"` — **eleven** fractional digits. 349 of the 1022 rows are padded past 1e-9, and an exact reader refuses every one of them |
| `THEUSDT` | `indexPrice` `"0.07308125"` against a 1e-5 tick: 7308.125 steps, which is not a number of steps. 446 of 1022 rows are like this, and it is the reason no index price is carried anywhere in this adapter. Its `markPrice` **does** divide, which is why that one is kept |

Two more public endpoints were captured while establishing the adapter and have
**no fixture and no reader**, because nothing would call them and a parser with
no caller is a facility nobody exercises and everybody trusts:

- `GET /capi/v3/market/depth?symbol=…&limit=15` answers `{asks, bids,
  lastUpdateId}` with no symbol and no timestamp. No book needs it: the depth
  channel opens with its own snapshot and reopens with another.
- `GET /capi/v3/market/time` answers `{"serverTime":…}`. A venue clock matters
  to a signature, and nothing in this adapter signs.

## WebSocket

The depth and trade files came out of one 75 s session subscribed to
`BTCUSDT@depth15`, `ETHUSDT@depth15`, `SOLUSDT@depth15`, `DOGEUSDT@depth15`,
`BTCUSDT@trade`, `ETHUSDT@trade`, `DOGEUSDT@trade`, `BTCUSDT@ticker`,
`ETHUSDT@ticker` and `DOGEUSDT@ticker` — 1063 frames.

| file | provenance |
| --- | --- |
| `ws_connected.json` | The greeting WEEX opens every socket with, before any subscribe. Carries a connection id and nothing else. |
| `ws_ping.json` | `{"event":"ping","time":"…"}`, about every 30 s per socket. The answer is the constant `{"method":"PONG","id":1}`; WEEX closes a connection that misses more than ten. |
| `ws_subscribe_ack.json` | `{"result":true,"id":1}`. |
| `ws_subscribe_refused.json` | `params:["BTCUSDT@bookTicker"]` on a fresh socket. **One boolean** separates this from the ack, and there is no numeric code — the reason is prose behind an `INVALID_ARGUMENT: ` prefix. |
| `ws_depth_snapshot.json` | The first frame of `BTCUSDT@depth15`: `e:"depthSnapshot"`, `d:"SNAPSHOT"`, 15 levels a side. This IS the seed — no REST read is involved. |
| `ws_depth_update.json`, `ws_depth_update_next.json` | The next two frames of the same channel, **consecutive**, chosen so each one's `U` equals the previous frame's `u`. The second deletes `79346.5` by sending it at quantity `"0"` — which is how WEEX keeps the window at fifteen. |
| `ws_trade_buy.json` | `BTCUSDT@trade`, both prints `m: false`. |
| `ws_trade_sell.json` | `DOGEUSDT@trade`, all three prints `m: true`. DOGE also carries the negative-precision size step: `"1500"` against a lot of 100. |
| `ws_trade_snapshot.json` | The opening `e:"tradeSnapshot"` backfill, twenty historical prints. Kept as **evidence, not as a published channel** — see below. |
| `ws_ticker.json` | `BTCUSDT@ticker`. Fourteen fields, and among them no bid, no ask and no funding rate. |

### Channels probed, and what the venue said

Subscribed one per frame on a fresh socket and read the reply:

| channel | result |
| --- | --- |
| `<sym>@depth15`, `<sym>@depth200` | accepted |
| `<sym>@depth` | accepted, and answers at `l: 15` |
| `<sym>@depth5`, `<sym>@depth20` | `INVALID_ARGUMENT: invalid depth level : 5` / `: 20` |
| `<sym>@trade`, `<sym>@ticker` | accepted |
| `<sym>@bookTicker` | `INVALID_ARGUMENT: invalid event : bookticker` |
| `<sym>@markPrice`, `<sym>@indexPrice`, `<sym>@premiumIndex` | `invalid event : markprice` / `indexprice` / `premiumindex` |
| `<sym>@fundingRate` | `invalid event : fundingrate` |
| `<sym>@miniTicker`, `<sym>@aggTrade` | `invalid event : miniticker` / `aggtrade` |
| `NOPEUSDT@trade` | `INVALID_ARGUMENT: invalid symbol : cmt_nopeusdt` — WEEX prefixes the symbol internally |

Three consequences the adapter is built around, and each is pinned by a test:

- **There is no quote channel.** `@bookTicker` is refused by name, and the
  ticker's fourteen fields carry no bid and no ask. The top of book is the
  ladder's first level or there is none, which is why `SubKind::Bbo` and
  `SubKind::Depth` resolve to one channel.
- **There is no funding channel.** The rate is polled from
  `/capi/v3/market/premiumIndex`, one call covering every contract.
- **A refusal takes the whole frame with it.** A `params` array of eight with
  one bad member left all eight unsubscribed and pushed nothing, so a frame is
  built only from channels the catalogue vouched for.

Case does not matter on either half: `btcusdt@depth15` and `BTCUSDT@DEPTH15`
were both accepted and both answered with `"s":"BTCUSDT"`.

### The depth chain

`U` equals the **previous frame's `u`**, not `u + 1`. Measured over 478
consecutive deltas across four books in 75 s: the shared-boundary rule held on
478 of 478 and the successor rule on 0 of 478, with no id skipped and none
repeated. The book stood at exactly 15 levels a side on 595 of 595 frames
applied, and every price and every size divided exactly by its contract's own
step on 6092 of 6092 levels.

A re-SUBSCRIBE of a channel a live socket already holds draws a fresh
`depthSnapshot` within 300 ms. That is the recovery a gap would need; the
connection pool has no per-key resend, so a gapped book is invalidated and waits
for its socket's next open, which re-asks and is answered the same way.

### Trade side

`m` says the **buyer was the maker**, so `m: true` means the aggressor SOLD.
Established by correlating live prints against the 15-level book from the same
session: `m: true` printed at or below the best bid **372** times and at or above
the best ask 33; `m: false` printed at or above the ask **596** times and at or
below the bid 29. The minorities are the book being a frame behind the print —
no print landed mid-spread.

### Why the trade backfill is dropped

`tradeSnapshot` is up to twenty historical prints, and the live frames that
follow **re-send part of that window**: five ids came round twice across two
sessions. The live stream itself repeated an id **0 times in 1457 prints**, so
`trade` needs no deduplication — but nothing downstream deduplicates a trade
either, which means publishing the backfill counts those prints twice in every
signed-flow series that begins at a reconnect. `t` is a UUID, not a number; read
as an integer it is zero on every print.

## Rate metering

`api-contract.weex.com` states the budget in `x-remaining-weight-10s` and
`x-used-weight-10s`: **500 weight in ten seconds**, per IP, counting back up from
1 after a pause. Every public call this adapter makes costs exactly 1 — the
685 KiB `exchangeInfo` and the 229 KiB `premiumIndex` cost what the 28-byte
clock read does.

The header and the body **disagree**, and `config/venues.yaml` follows the
header. `exchangeInfo`'s own `rateLimits` array says `REQUEST_WEIGHT` / interval
`MINUTE` / `intervalNum` 10 / `limit` 500, which reads as 500 per ten *minutes*
— sixty times stingier than what the venue enforces.

## Not captured

There are no credentials for this venue in this repository, so nothing private
was captured and nothing private is implemented. The caps row declares order
entry, the user stream and every reconciliation pull false, and the execution
client refuses each command locally rather than leaving it open.
