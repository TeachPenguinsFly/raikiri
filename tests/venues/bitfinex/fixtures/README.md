# bitfinex fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file in the PUBLIC sections below was **captured live** from production
Bitfinex on 2026-08-11 — public endpoints and the public websocket, no
credentials involved, and nothing hand-written or reshaped from docs.

Order entry cannot be captured without credentials. The **private** section at
the end says, file by file, which bytes came off the wire anyway and which were
assembled from Bitfinex's own SDK models.

Files were spliced out of the captures **textually** (`sed -n 'Np'`), never
re-serialized through a JSON library. A round trip rewrites `1e-8` as `1.0e-08`
and `0.0002` as `2e-04`, and those exact spellings are what several tests
exist to pin: Bitfinex mixes plain decimals and scientific notation inside one
payload, which is the reason this adapter carries an exponent-aware parser at
all.

## Websocket — `wss://api-pub.bitfinex.com/ws/2`

Six sessions. Bitfinex mints a channel id per connection, so every capture has
its own — which is itself the thing `ChannelIdsDifferPerConnection` pins.

### Session 1 — `ticker` on `tBTCUSD` (chanId 862367)

| file | source |
|---|---|
| `ws_info.json` | the greeting every connection opens with |
| `ws_subscribed_ticker.json` | the subscribe acknowledgement — the only frame that ties 862367 to `tBTCUSD` |
| `ws_ticker.json` | first ticker push |
| `ws_heartbeat.json` | `[862367,"hb"]` |

### Session 2 — `trades` on `tBTCUSD` (chanId 694830)

| file | source |
|---|---|
| `ws_subscribed_trades.json` | acknowledgement |
| `ws_trades_snapshot.json` | the 30-print history served on subscribe, untrimmed |
| `ws_trade_te.json` | a `te` execution, amount `-0.0002` at `63951` |
| `ws_trade_tu.json` | the `tu` restatement of **that same print** — same id 1956671261, same time, same size, same price |

`te` and `tu` being one trade twice is why only `te` is published; the pair is
kept together deliberately so the test can assert it rather than assume it.

### Session 3 — `book` on `tBTCUSD`, `P0/F0/len=25` (chanId 722232)

| file | source |
|---|---|
| `ws_subscribed_book.json` | acknowledgement |
| `ws_book_snapshot.json` | the snapshot, 50 levels (25 a side), untrimmed |
| `ws_book_delete.json` | `[722232,[63951,0,1]]` — COUNT 0 removes the level, AMOUNT `+1` says it was a bid |
| `ws_book_delete_ask.json` | `[722232,[63963,0,-1]]` — the same, on the ask side |
| `ws_book_update.json` | an ordinary level update |
| `ws_book_snapshot_len100.json` | tBTCF0:USTF0 at `len:"100"`, captured 2026-08-17 — 200 levels, 100 a side, which is what the adapter now subscribes at |

### Session 4 — `status` keyed `deriv:tBTCF0:USTF0` (chanId 782668)

| file | source |
|---|---|
| `ws_subscribed_status.json` | acknowledgement. Note it carries **`key` and no `symbol`** — this channel names its instrument nowhere else |
| `ws_deriv_status.json` | one status push: 23 slots, 15 of them `null` |

### Session 5 — two bad subscribes

| file | source |
|---|---|
| `ws_error_symbol.json` | `{"event":"subscribe","channel":"ticker","symbol":"tNOSUCHSYM"}` |
| `ws_error_channel.json` | a channel that does not exist |

Both answered code **10300**, not the 10301 the task brief expected; 10300 is
Bitfinex's generic "subscription failed" and it does not distinguish a bad
symbol from a bad channel. Only `msg` does.

### Session 6 — `conf` flags 32768 (TIMESTAMP) then subscribes (chanIds 707879, 607642)

| file | source |
|---|---|
| `ws_conf_ack.json` | `{"event":"conf","status":"OK","flags":32768}` |
| `ws_pong.json` | the answer to `{"event":"ping","cid":7788}` |
| `ws_ticker_timestamped.json` | a ticker frame with the venue send time appended **after** the payload |
| `ws_trade_te_timestamped.json` | a `te` frame with the timestamp behind the payload, i.e. at index 3 |
| `ws_heartbeat_timestamped.json` | `[607642,"hb",1786431367792]` — even heartbeats grow a field |
| `ws_trade_te_scientific.json` | a `te` whose AMOUNT is spelled `1e-8` |

The flag is requested by the adapter because the `ticker` payload carries no
timestamp of its own. Sessions 1–5 were captured without it, so the same
parsers are exercised against both envelope shapes.

## REST — `https://api-pub.bitfinex.com`

| file | source |
|---|---|
| `rest_platform_status.json` | `GET /v2/platform/status` → `[1]` |
| `rest_error.json` | `GET /v2/ticker/tNOSUCHSYM` → `["error",10020,"symbol: invalid"]`, an array like everything else |
| `rest_ticker.json` | `GET /v2/ticker/tBTCUSD` |
| `rest_conf_pair.json` | `GET /v2/conf/pub:info:pair` (182 rows) trimmed to four |
| `rest_conf_pair_futures.json` | `GET /v2/conf/pub:info:pair:futures` (90 rows) trimmed to four |

Both catalogue files keep their real three-deep nesting. The four rows in each
were chosen to cover what the mapper has to decide:

* `BTCUSD` / `BTCF0:USTF0` — the pairs every websocket capture above came from.
* `ETHUSD` / `ETHF0:USTF0` — a second ordinary row.
* `AAVE:USD` — a spot row whose margin slots are `null` where BTCUSD's are
  numbers, so the reader cannot lean on "the tail is always numeric".
* `AUSTRALIA200IXF0:USTF0` — the longest symbol Bitfinex lists (22 chars, 23
  with the `t` prefix) and a non-crypto index perpetual.
* `TESTBTC:TESTUSD` / `TESTBTCF0:TESTUSDTF0` — **the rows the mapper must
  refuse.** These are paper-trading instruments served from the same
  production endpoint as the real ones (28 of 272 rows across both markets).
  They quote simulated prices and are reachable only from a paper sub-account,
  so loading one puts a fabricated book in a live catalogue.

Note what these rows do **not** contain: no price tick, no amount step, no
status or listing field. `MIN_ORDER_SIZE` and `MAX_ORDER_SIZE` are the only
numbers, and both are bounds rather than grids. That absence is the evidence
behind the derived step in `map.hpp`.

`rest_ticker.json` is kept because the REST ticker's eleventh slot is an
integer where the websocket's is `null` — the clearest single illustration
that the documented ten-field ticker is not what either transport sends.


## Private — order entry

Bitfinex serves the authenticated half from a DIFFERENT host,
`api.bitfinex.com`, and every authenticated call is a POST with a JSON body:
no GETs, no query strings. The private websocket is the same
`api-pub.bitfinex.com/ws/2` socket after an `auth` event.

### Captured live, unauthenticated

| file | how |
| --- | --- |
| `rest_auth_error.json` | `POST api.bitfinex.com/v2/auth/r/orders` with no headers at all |

That one file earns two tests. Its code is `10100`, which classifies as
AuthFailure — and **the HTTP status beside it is 500**, so an adapter that read
the status code would call a bad key a venue outage and retry into it.

The five endpoints this adapter posts to were each probed the same way on
2026-08-26 and each answered `["error",10100,"apikey: invalid"]`, while
`/v2/auth/r/nope` answered a **404 with an HTML body** — which is what makes
the probe evidence that the paths are right rather than merely reachable:

    /v2/auth/w/order/submit   /v2/auth/w/order/cancel   /v2/auth/w/order/update
    /v2/auth/r/orders         /v2/auth/r/positions      /v2/auth/r/trades/hist

### Built from Bitfinex's own models

No credentials exist to capture these, so they are assembled from the field
maps in Bitfinex's own SDK — `bfx-api-node-models/lib/{order,trade,position,
notification}.js`, which is the authoritative statement of these positional
schemas — and marked as such. Every INDEX is the model's; the symbols, prices
and sizes were chosen to be ones `rest_conf_pair.json` above actually lists, so
the mapper divides by the same derived 1e-8 grid the live captures do.

| file | shape |
| --- | --- |
| `ws_auth_ok.json`, `ws_auth_failed.json` | the `auth` acknowledgement, which is an OBJECT where every push is an array |
| `ws_order_new.json` | `[0,"on",[...]]`, an ACTIVE spot buy |
| `ws_order_executed.json` | `[0,"oc",[...]]` at `EXECUTED @ 63951.0(-0.1)` |
| `ws_order_canceled.json` | the same order at `CANCELED` |
| `ws_order_postonly_canceled.json` | a perpetual at `POSTONLY CANCELED`, flags 4096 |
| `ws_orders_snapshot.json` | `[0,"os",[[...],[...],[...]]]` across both markets |
| `ws_exec_te.json`, `ws_exec_tu.json` | the SAME fill twice — see below |
| `ws_positions_snapshot.json` | `[0,"ps",[...]]`, one ACTIVE short and one CLOSED row |
| `ws_notification_error.json` | `[0,"n",[...]]`, a refused submit |
| `rest_order_submit.json`, `rest_order_error.json` | the notification a write answers with, at SUCCESS and at ERROR |
| `rest_order_cancel.json` | the `oc-req` notification |
| `rest_orders.json`, `rest_trades.json`, `rest_positions.json` | the same rows bare, with no socket envelope |

Five properties these carry, each pinned by a test rather than left as a
comment:

- **A fully executed order has AMOUNT 0** (`ws_order_executed.json`), and zero
  has no sign. `AMOUNT_ORIG` is the only field that still says which way the
  order went, and a reader taking the side from `AMOUNT` reports every
  completed sale as a buy.
- **`POSTONLY CANCELED` does not start with `CANCELED`.** The status is prose
  with the fill glued on, so it is matched by prefix; equality would leave a
  post-only refusal sitting open forever.
- **`te` and `tu` are the same fill twice.** `te` arrives immediately with FEE
  null and `tu` restates it once the fee is known — same id, same order id,
  same amount. Publishing both double-counts every fill and every fee, so only
  `tu` reaches the sink.
- **The fee is NEGATIVE when charged** (`-0.0639` in `ws_exec_tu.json`), which
  is the opposite of every other venue in this repo, so the sign is flipped
  once on the way in.
- **`rest_positions.json` holds a margin position on a SPOT pair**
  (`tBTCUSD`). This adapter opens none — every spot order it sends carries the
  `EXCHANGE ` prefix — so that row is dropped rather than reported against a
  Spot instrument id, which would claim a position on a market that has no
  such concept here.

### Signature vectors

Bitfinex publishes none, so the algorithm and the message shape are pinned
separately. HMAC-SHA384 is checked against **RFC 4231 test case 1**, and a
second test asserts it is not SHA-512 truncated — SHA-384 has a different
initial state, so a caller reaching for the SHA-512 already in `libs/net` would
produce a digest the venue answers `10100` to. The message shapes come from
Bitfinex's own SDK (`bfx-api-node-util/lib/gen_auth_sig.js` and
`bfx-api-node-rest/lib/rest2.js`): `/api` + path + nonce + body for REST, and
`AUTH` + nonce + **the same nonce again** for the socket.
