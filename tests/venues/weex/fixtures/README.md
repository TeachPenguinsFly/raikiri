# WEEX fixtures

The **public** files here were captured from production WEEX on **2026-09-07**.
Nothing in that half is hand-written or synthesised: two files are edited
captures and say so, and the rest are verbatim response bodies and websocket
frames, reformatted only by being written as one line.

The **private** files are a different kind of evidence and are kept in a section
of their own below. There are no WEEX credentials in this repository, so no
successful private body has ever been seen — those fixtures are WEEX's own
documented examples, and the file that lists them says which. The only private
answers that ARE captures are the refusals, taken unauthenticated on
**2026-09-13**.

Every file below is the USDT-margined **contract** tree on
`api-contract.weex.com`, which serves `/capi/v3/...`. WEEX's spot exchange is a
different host with a different prefix — `api-spot.weex.com` serving
`/api/v3/...` — and the two do not cross. Measured on 2026-09-13: `GET
api-contract.weex.com/api/v3/ping` → **404** and `GET
api-spot.weex.com/capi/v3/market/time` → **404**, while each host answers its
own prefix. It is one exchange behind one API key — the same key page, the same
three credentials, the same signature rule, with a `Spot` or a
`Futures/Contract` permission ticked on it — but it is two hosts, and this
adapter reaches one of them. That is why `caps.markets.spot` stays false: not
because spot is another company, but because no code here addresses that host.

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
| `ws_depth200_snapshot.json`, `ws_depth200_update.json` | `BTCUSDT@depth200` on 2026-09-26: the opening snapshot at `l:200` with 200 levels a side, and the delta that chains onto it. The adapter subscribes this tier and publishes the top 100. |
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

## Private

**No credential has reached this venue from this machine.** That fact shapes
everything in this section, and it is the reason the caps row claims none of the
private surface however much of it is implemented.

### Captured — the refusals, and only the refusals

Both taken from production on **2026-09-13** with no headers at all. They are
the only WEEX private answers anywhere in this repository, and what they prove
is narrow but real: that these paths are ADDRESSES, and what a refusal looks
like.

| file | provenance |
| --- | --- |
| `rest_error_unauthorized.json` | `GET /capi/v3/account/balance` with no ACCESS-* headers: `{"code":-1044,"msg":"Invalid ACCESS_KEY."}` at **HTTP 401**. Every private path listed below answered byte-identically. |
| `rest_error_bad_body.json` | `POST /capi/v3/order` with an empty body: `{"code":-1135,"msg":"Request parameter format is incorrect"}` at **HTTP 400** — the format check runs BEFORE the credential check, which is how an unsigned probe reached it. |

Every path this adapter signs was probed the same way and answered `-1044` at
401: `/capi/v3/order` (GET and DELETE), `/capi/v3/openOrders`,
`/capi/v3/order/history`, `/capi/v3/userTrades`,
`/capi/v3/account/position/allPosition`, `/capi/v3/account/balance` and
`/capi/v3/account/income`. A path WEEX does **not** route answers the other
shape entirely — `{"timestamp","status":404,"error":"Not Found","path":...}` —
which is how the two were told apart. `/capi/v3/nonsenseEndpointXyz` was the
control.

The older `/capi/v2/` tree is still live on this host and is a **different API**
that WEEX's changelog says is being retired. Measured the same day: it answers
`{"code":"40006","msg":"无效的ACCESS_KEY", ...}` — the code is a STRING, the
message is Chinese — and `GET /capi/v2/market/contracts` lists `cmt_btcusdt`
rather than `BTCUSDT`. Nothing here touches it, and a reader who finds a WEEX
integration elsewhere shaped like that is reading the old API.

### Documented — the success shapes

These are WEEX's own response examples from its endpoint pages, written as one
line. **They are not captures.** Where a case the venue does not print was
needed, the row is an edit of the venue's own and says so here.

| file | source page | edited? |
| --- | --- | --- |
| `rest_order_ack.json` | Place Order (`POST /capi/v3/order`) | `clientOrderId` replaced with a real 26-char ULID, because attribution is what the test is about |
| `rest_cancel_ack.json` | Cancel Order (`DELETE /capi/v3/order`) | same, and note the field is `origClientOrderId` here and `clientOrderId` on the place answer |
| `rest_cancel_batch.json` | Cancel Orders Batch (`DELETE /capi/v3/batchOrders`) | verbatim but for the two ULIDs. **One entry succeeds and one fails with `"errorCode":"ORDER_NOT_FOUND"`** — that is the venue's own example, and it is the normal shape of a batch rather than a failure of the request |
| `rest_open_orders.json` | Get Current Orders (`GET /capi/v3/openOrders`) | row 1 is the venue's, with a ULID. **Row 2 is an edit**: `executedQty` 0.008 of an `origQty` 0.02, `status` still `"NEW"` — WEEX prints no partially filled example because its status vocabulary has none. This is also where the order-id-as-a-number case lives |
| `rest_user_trades.json` | Get Trade Details (`GET /capi/v3/userTrades`) | verbatim |
| `rest_positions.json` | Get All Positions (`GET /capi/v3/account/position/allPosition`) | row 1 is the venue's. **Rows 2 and 3 are edits**: a second BTCUSDT row on the opposite side, which is what `separatedMode` SEPARATED produces and what the netting exists for, and a DOGEUSDT row to prove the lot step of 100 |
| `rest_balance.json` | Get Account Balance (`GET /capi/v3/account/balance`) | verbatim |
| `rest_income.json` | Get Account Income (`POST /capi/v3/account/income`) | row 1 is the venue's own `position_funding` example. **Rows 2 and 3 are edits**: a settlement with a NEGATIVE `income`, and a `transfer_in` that must not be booked as funding |

### What the documentation itself gets wrong

Recorded because each one is a trap the adapter is built around:

- **An order id arrives as two JSON types.** The place and cancel answers quote
  it as a string; the order, open-order and fill reads quote it as an 18-digit
  number. WEEX's own page shows the cost: its open-order example prints
  `702345678901234600` for the order its place example called
  `702345678901234567`, having lost three digits to a double. The adapter reads
  the id off the raw token as text, never as a float.
- **There is no `PARTIALLY_FILLED`.** The whole vocabulary is `NEW`, `PENDING`,
  `UNTRIGGERED`, `UNACTIVATED`, `FILLED`, `CANCELED`, `CANCELING`. A half-filled
  order is `NEW` and only `executedQty` against `origQty` says otherwise.
- **The single-order read and the order history answer the same object as the
  open-order list**, and neither is built: WEEX looks an order up by `orderId`
  only, and this process does not know the venue's id for an order it has lost.
  So `query_order` goes through the open-order list, and there is no parser for
  a shape nothing fetches.
- **A position states no entry price.** `openValue` and `size` are both there
  and the quotient is not a price the venue quoted, so `avg_entry` stays UNDEF.
- **`positionSide` is required and has no "both" value.** The adapter derives it
  — an opening order faces the way it trades, a reduce-only order faces the
  position it closes — and that derivation is the single most likely thing here
  to be wrong the first time a credential reaches the venue.
- **A 200 is not an acceptance.** A refused order arrives inside a successful
  HTTP response with `success` false and a NAMED `errorCode` (a string like
  `ORDER_NOT_FOUND`), not one of the numeric codes.
- **The `-1180` error text contradicts the endpoint pages**: it says
  `client_oid` may not exceed 40, naming a field V3 does not have, while every
  endpoint page states 1-36 and the pattern `^[\.A-Z\:/a-z0-9_-]{1,36}$`. The
  adapter treats 36 as binding.
- **The contract `RequestInteraction` page is a copy of the spot page**, with
  `api-spot.weex.com` and `/api/v3/order` hardcoded in its sample code. The
  Signature page and the per-endpoint curl examples are the ones to read.

### Order rate

The venue's own `exchangeInfo` states it, read live on 2026-09-13:

```json
"rateLimits": [
  {"rateLimitType":"REQUEST_WEIGHT","interval":"MINUTE","intervalNum":10,"limit":500},
  {"rateLimitType":"ORDERS","interval":"MINUTE","intervalNum":1,"limit":300}
]
```

Orders are metered **per account**, cost **zero IP weight**, and are capped at
**300 a minute**. No per-second figure is published anywhere — the
`X-ORDER-COUNT-10S` bucket exists in the response headers but its ceiling is
never stated — which is why the caps row's `headline_order_rate_per_s` stays 0
rather than carrying a quotient.

### Not implemented

The private **websocket** (`wss://ws-contract.weex.com/v3/ws/private`, with
`orders`, `fill`, `positions` and `account` channels, authenticated by ACCESS-*
headers on the handshake and with no listen key anywhere). Nothing here opens
it, so **a fill reaches this process through the reconciliation pull and nowhere
else**, and an order accepted over REST is reported Accepted whatever it then
did. Four push parsers nobody could exercise would be four parsers that are
wrong when they finally are.
