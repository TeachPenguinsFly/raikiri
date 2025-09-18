# XT fixtures

XT is two exchanges under one brand and this directory reflects that: the
futures tree answers from `fapi.xt.com` / `fstream.xt.com` and the spot tree
from `sapi.xt.com` / `stream.xt.com`, and they share neither an envelope, a
signature scheme, nor a field vocabulary.

Everything in the **public** sections below was captured from **production XT**
— the futures files on 2026-08-10/11, the spot files on 2026-08-26. Nothing
there is hand-written or synthesised. Two futures files are edited captures and
say so; the rest are verbatim response bodies and websocket frames, reformatted
only by being written as one compact line.

Order entry cannot be captured without credentials. The **private** section at
the end says, file by file, which bytes came off the wire anyway and which were
assembled from XT's own reference.

- REST: `https://fapi.xt.com`, plain `curl`.
- WS: `wss://fstream.xt.com/ws/market`, `websocat -B 200000`, piped through
  `head -N` (websocat cannot redirect straight to a file).

Frames arrive as **uncompressed UTF-8 JSON**. No binary-frame support is needed
anywhere in this adapter.

## REST

| file | provenance |
| --- | --- |
| `rest_server_time.json` | `GET /future/market/v1/public/time`. Note `result` is a bare number, not an object. |
| `rest_depth.json` | `GET /future/market/v1/public/q/depth?symbol=btc_usdt&level=5`. |
| `rest_funding_rate.json` | `GET /future/market/v1/public/q/funding-rate?symbol=btc_usdt`. `fundingRate` is a JSON **number**. |
| `rest_funding_rate_plain.json` | Same endpoint, `eth_usdt`. Kept as a second sample of the number field. |
| `rest_futures_bills.json` | **SIGNED, live**: `GET /future/user/v1/balance/bills`. Four rows chosen from a 100-row capture — a FUND that was received, a FUND that was paid, a FEE, and a CLOSE_POSITION the mapper must refuse. `accountId` is replaced with a placeholder; no parser reads it. |
| `rest_error_invalid_symbol.json` | Same endpoint with `symbol=nope_usdt`. |
| `rest_error_invalid_param.json` | `.../q/depth?symbol=btc_usdt&level=abc`. |
| `rest_symbol_list.json` | **EDITED**: `GET /future/market/v1/public/symbol/list` returned 1067 rows (2.2 MiB); five untouched rows were kept and the envelope rebuilt around them. Field contents are verbatim. |

The five catalogue rows were chosen so each isolates exactly one mapper
decision — otherwise a row refused for two reasons proves nothing about either:

| symbol | outcome | why |
| --- | --- | --- |
| `btc_usdt` | accepted | tick 0.1, contractSize 0.0001, minNotional 10 |
| `eth_usdt` | accepted | tick 0.01, contractSize 0.01 |
| `ftt_usdt` | refused | `tradeSwitch: false` — but `isOpenApi: true`, so it isolates the trade gate |
| `dia_usdt` | refused | `isOpenApi: false` — but `tradeSwitch: true`, so it isolates the API gate |
| `btc_usdt_261225` | refused | passes every switch; refused only for `productType: "futures"` |

There is **no non-online state row**, because there is no such state: `state`
was `0` on all 1067 rows. The switches are the real tradability gate, and they
split the catalogue roughly in half (624 rows pass `state==0 && tradeSwitch &&
isOpenApi`, 620 of them perpetuals).

Also absent by necessity: a row exercising the sub-nanosecond tick limit.
`1000kishu_usdt` lists `minStepPrice: "0.0000000001"`, finer than `FixedE9`
holds — but its switches are off, so it is already refused a step earlier. No
currently tradable perp has a tick finer than 1e-9.

## WebSocket

| file | provenance |
| --- | --- |
| `ws_depth.json` | `depth@btc_usdt,50,100ms`, recaptured 2026-08-17. The BBO **and** book source: a self-contained top-N snapshot with sizes. 50 is XT's deepest tier — `60`, `80`, `99`, `100` and `200` each drew `{"code":1,"msg":"levels error"}` on a fresh socket. |
| `ws_depth_update.json`, `ws_depth_update_next.json` | `depth_update@btc_usdt`, two **consecutive** frames chosen so the second's `pu` equals the first's `u`. |
| `ws_trade_buy.json` | `trade@btc_usdt` with `m: "BID"`. |
| `ws_trade_sell.json` | `trade@btc_usdt` with `m: "ASK"`. |
| `ws_mark_price.json` | `mark_price@btc_usdt`. |
| `ws_agg_ticker.json` | `agg_ticker@btc_usdt`. Kept as **evidence**, not as an implemented channel: it carries `bp`/`ap` and no sizes, which is why it cannot fill a `Bbo`. |
| `ws_subscribe_ack.json` | The `{"code":0,"msg":"success","id":...}` XT answers an accepted subscribe with. |
| `ws_error_unknown_topic.json` | The refusal for `funding_rate@btc_usdt`. This is the proof that XT streams no funding channel. |

### Topics probed, and what the venue said

Subscribed one per frame and read the reply:

| topic | result |
| --- | --- |
| `depth@sym,5,100ms` | accepted; ~7–10 frames/s |
| `depth@sym,{5,10,20,50}` | accepted; ~1 frame/s without an interval |
| `depth@sym,1` | `levels error` |
| `depth@sym,5,50ms` | `interval error` |
| `depth@sym` | `args error` — the level is mandatory |
| `depth_update@sym` | accepted; echoed back as `depth_update@sym,100ms` |
| `trade@sym`, `mark_price@sym`, `agg_ticker@sym`, `ticker@sym`, `index_price@sym` | accepted |
| `kline@sym` | `args error` — needs a bar size |
| `bbo@`, `book_ticker@`, `agg_trade@`, `funding@`, `fundingRate@`, `funding_rate@` | `No enum constant com.xt.thanos.biz.websocket.data.Topic.<name>` |

### Trade side

`m` is the **aggressor's** side, not the maker's. Established by correlating 47
prints against a 100 ms `depth` snapshot across two sessions: `m: "ASK"` printed
at or below the best bid 19 times and at or above the best ask **zero** times;
`m: "BID"` printed at or above the best ask and never at or below the bid. The
remainder landed mid-spread, which is book staleness between snapshots.


## Spot public — `https://sapi.xt.com`

| file | provenance |
| --- | --- |
| `rest_spot_server_time.json` | `GET /v4/public/time`. `result` is an OBJECT with `serverTime` — unlike the futures clock, whose `result` is the bare number. |
| `rest_spot_symbol_list.json` | **EDITED**: `GET /v4/public/symbol` returned 1103 rows; six were kept VERBATIM (sliced out of the response by brace matching, never re-serialised) and the envelope rebuilt around them. |
| `rest_spot_error_auth.json` | `GET /v4/open-order` with no headers at all. |
| `rest_spot_error_not_found.json` | `GET /v4/nope`. |

The six catalogue rows were chosen so each isolates one mapper decision:

| symbol | outcome | why |
| --- | --- | --- |
| `btc_usdt` | accepted | `pricePrecision` 2, `quantityPrecision` 5, `QUOTE_QTY` min `"1"` |
| `eth_usdt` | accepted | a second accepted row with a different quantity scale |
| `sss_usdt` | refused | `tradingEnabled: false` — `openapiEnabled` is true and state is ONLINE, so it isolates the trade gate |
| `btx_usdt` | refused | `openapiEnabled: false` — `tradingEnabled` is true, so it isolates the API gate |
| `sats_usdt` | refused | `pricePrecision: 11`, finer than `FixedE9` carries. Every switch is on, so the refusal is attributable to the scale alone |
| `tora_usdt` | refused | `state: "OFFLINE"` |

`tora_usdt` is the one row that does NOT isolate its reason: **no** row in the
1103 was offline with both switches on, so an OFFLINE row that isolates `state`
does not exist to capture. 1040 rows were ONLINE, 1083 had `tradingEnabled` and
only **496** had `openapiEnabled` — the API gate is the one that actually halves
the catalogue, exactly as `isOpenApi` does on the futures side.

`rest_spot_error_auth.json` earns its place twice over: it is the evidence
behind `classify_spot`, and it is the proof that **spot answers a refusal at
HTTP 200**. `rc` is the only thing that says the call failed, so any code path
that treats a 2xx as success on this tree is wrong. `rest_spot_error_not_found.json`
is the one refusal that does carry a non-200 status.

## Private — order entry

No credentials exist in this repository, so these files split into two
provenances and each one says which it is.

### Captured live, unauthenticated

| file | how |
| --- | --- |
| `rest_spot_error_auth.json` | see above — `{"rc":1,"mc":"AUTH_001"}` at HTTP 200 |
| `rest_futures_error_no_key.json` | `POST fapi.xt.com/future/trade/v1/order/create` with no headers: `{"returnCode":1,...,"error":{"code":"400"}}` |

Every futures order route was probed unauthenticated on 2026-08-26 and each one
answered the same `400 Absence of [appKey]`. That is **not** proof the routes
exist — `fapi.xt.com` runs its auth filter before routing, and a deliberately
bogus path answered identically. The spot routes ARE proven that way:
`sapi.xt.com` routes first, so `/v4/nope` came back 404 while `/v4/order`,
`/v4/order/{id}` (PUT and DELETE), `/v4/batch-order`, `/v4/open-order`,
`/v4/trade`, `/v4/balances` and `/v4/ws-token` all answered `AUTH_001`.

### Built from XT's own reference

Assembled from the payload examples in `github.com/XtApis/xt-api` (the docs
repo behind doc.xt.com). Field NAMES and TYPES are the doc's; symbols, prices
and quantities were changed to values the catalogue fixtures above actually
carry, so the mapper divides by real tick and lot sizes. `clientOrderId` is
`02WP8qFtGJTkHk10j5u4aZ`, which is the ULID `01ARZ3NDEKTSV4RRFFQ69G5FAV` in the
22-character form XT's `^[a-zA-Z0-9_]{4,22}$` field can hold.

| file | reference shape |
| --- | --- |
| `rest_futures_open_orders.json` | `POST /future/trade/v1/order/list-open-order`, two rows: one NEW, one PARTIALLY_FILLED |
| `rest_futures_order_list.json` | `POST /future/trade/v1/order/OrderList` — the PAGED form, `result.items` |
| `rest_futures_trade_list.json` | `GET /future/trade/v1/order/trade-list`, one MAKER row and one TAKER row |
| `rest_futures_positions.json` | `GET /future/user/v1/position/list`, one SHORT and one flat |
| `rest_futures_balance_list.json` | `GET /future/user/v1/balance/list`, four rows: a quoted one, a bare-number one, one with no `availableBalance`, and the doc's own coinless row |
| `rest_futures_order_ack.json` | the create/cancel answer, whose `result` is a BARE STRING |
| `rest_listen_key.json` | `GET /future/user/v1/user/listen-key` |
| `rest_ws_token.json` | `POST /v4/ws-token` — the doc's own (long expired) sample `accessToken`, verbatim |
| `rest_spot_order_ack.json`, `rest_spot_cancel_ack.json` | `POST /v4/order` and `DELETE /v4/order/{id}`: `orderId` on one, `cancelId` on the other |
| `rest_spot_open_orders.json` | `GET /v4/open-order`, one partially filled and one placed elsewhere |
| `rest_spot_trades.json` | `GET /v4/trade`, `result.items` |
| `ws_futures_order_new.json`, `ws_futures_order_partially_canceled.json` | the `order@{listenKey}` push |
| `ws_futures_trade.json` | the `trade@{listenKey}` push |
| `ws_spot_order_new.json`, `ws_spot_order_partial.json` | the spot `order` push, two-letter keys |
| `ws_spot_trade.json` | the spot `trade` push |
| `ws_private_ack.json`, `ws_private_listen_key_invalid.json` | the subscribe answer at `code` 0 and at `code` 2 |

Five of these carry a property the adapter is built around, and each is pinned
by a test rather than left as a comment:

- **`rest_futures_order_list.json` quotes nothing.** `orderId`, `price` and
  `origQty` are JSON numbers there while `list-open-order` quotes all three.
  Its `orderId` is also `538511221008857280`, wider than a double holds
  exactly, which is why the raw token is kept rather than a parsed value.
- **`rest_futures_order_list.json` and the spot files echo the symbol in
  UPPERCASE** (`BTC_USDT`) where the catalogue said `btc_usdt`. A private
  lookup that does not fold case misses every record.
- **`ws_futures_order_new.json` carries no `clientOrderId` at all** — only the
  trade push and the REST reads do. It is the reason a memo keyed by venue
  order id exists.
- **`rest_futures_balance_list.json` mixes both number encodings in one
  body.** `usdt` quotes its figures and `btc` sends them bare, which is the
  difference between XT's live shape (quoted, per ccxt's recorded response) and
  XT's own doc example (bare). Its `usdt` walletBalance carries twelve
  fractional digits, past what `FixedE9` holds: an exact reader refuses the
  whole number and the account then shows nothing at all. Its third row omits
  `availableBalance` so undef stays distinguishable from zero, and its fourth
  keeps the doc's literal `"coin": ""` — a number with no unit, which the
  mapper drops rather than filing under an empty asset code.
- **`ws_spot_trade.json` carries no side.** `b` says whether the print's BUYER
  was the maker, which is a fact about the trade rather than about us; `tm`
  (1 taker, 2 maker) is our side of the BOOK, not our direction.

`rest_spot_open_orders.json`'s second row keeps a `clientOrderId` of
`myOrder123` — XT's own doc example, and not one of our encodings. It is the
evidence that a foreign id must leave the client-order-id field EMPTY instead
of dropping the record: an order we did not place is still one the reconciler
has to see.

### Signature vectors

The six vectors pinned in `venue_test.cpp` are not fixtures and are not XT's.
**XT's own published example signature does not reproduce** — that doc page
quotes two different appkeys — so the vectors were computed from XT's Go SDK's
construction (`xt-com/xt-go-demo`, `xt/v4/{spot,future}/utils.go`) and verified
independently. Three of them are reproduced end to end by the request builder;
the other three pin the scheme against `hmac_sha256_hex` directly.
