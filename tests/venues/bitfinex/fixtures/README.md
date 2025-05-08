# bitfinex fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file below was **captured live** from production Bitfinex on 2026-08-11.
No credentials are involved — all of these are public endpoints and the public
websocket. Nothing in this directory is hand-written or reshaped from docs.

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
