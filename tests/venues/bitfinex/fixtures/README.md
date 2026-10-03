# Bitfinex fixtures

The public files were captured from production Bitfinex on 2026-08-11, unless a
row gives another date, with no credentials. Each file was cut out of the
capture textually (`sed -n 'Np'`) without re-serialising, so spellings such as
`1e-8` and `0.0002` are exactly what the venue sent. The private files at the
end are mostly built from Bitfinex's SDK models and say so.

## Websocket: `wss://api-pub.bitfinex.com/ws/2`

Six sessions. Channel ids are minted per connection, so each session has its
own.

Session 1, `ticker` on `tBTCUSD` (chanId 862367):

| file | source |
|---|---|
| `ws_info.json` | the greeting sent on connect |
| `ws_subscribed_ticker.json` | the subscribe acknowledgement |
| `ws_ticker.json` | first ticker push |
| `ws_heartbeat.json` | `[862367,"hb"]` |

Session 2, `trades` on `tBTCUSD` (chanId 694830):

| file | source |
|---|---|
| `ws_subscribed_trades.json` | acknowledgement |
| `ws_trades_snapshot.json` | the 30-print history sent on subscribe, untrimmed |
| `ws_trade_te.json` | a `te` execution, amount `-0.0002` at `63951` |
| `ws_trade_tu.json` | the `tu` for the same print (id 1956671261) |

Session 3, `book` on `tBTCUSD`, `P0/F0/len=25` (chanId 722232):

| file | source |
|---|---|
| `ws_subscribed_book.json` | acknowledgement |
| `ws_book_snapshot.json` | the snapshot, 25 levels a side, untrimmed |
| `ws_book_delete.json` | `[722232,[63951,0,1]]`, a bid removal |
| `ws_book_delete_ask.json` | `[722232,[63963,0,-1]]`, an ask removal |
| `ws_book_update.json` | an ordinary level update |
| `ws_book_snapshot_len100.json` | `tBTCF0:USTF0` at `len:"100"`, captured 2026-08-17, 100 levels a side |

Session 4, `status` keyed `deriv:tBTCF0:USTF0` (chanId 782668):

| file | source |
|---|---|
| `ws_subscribed_status.json` | acknowledgement (carries `key`, no `symbol`) |
| `ws_deriv_status.json` | one status push |

Session 5, two bad subscribes:

| file | source |
|---|---|
| `ws_error_symbol.json` | `{"event":"subscribe","channel":"ticker","symbol":"tNOSUCHSYM"}` |
| `ws_error_channel.json` | a subscribe to a channel that does not exist |

Session 6, `conf` with flags 32768 (TIMESTAMP), then subscribes (chanIds 707879, 607642):

| file | source |
|---|---|
| `ws_conf_ack.json` | `{"event":"conf","status":"OK","flags":32768}` |
| `ws_pong.json` | the answer to `{"event":"ping","cid":7788}` |
| `ws_ticker_timestamped.json` | a ticker frame with the send time appended |
| `ws_trade_te_timestamped.json` | a `te` frame with the timestamp at index 3 |
| `ws_heartbeat_timestamped.json` | `[607642,"hb",1786431367792]` |
| `ws_trade_te_scientific.json` | a `te` whose amount is `1e-8` |

Not tied to a session above; capture date not recorded:

| file | source |
|---|---|
| `ws_subscribed_ticker_perp.json` | `ticker` acknowledgement for `tBTCF0:USTF0` (chanId 840202) |
| `ws_ticker_perp.json` | the first ticker push on that channel |

## REST: `https://api-pub.bitfinex.com`

| file | source |
|---|---|
| `rest_platform_status.json` | `GET /v2/platform/status` |
| `rest_error.json` | `GET /v2/ticker/tNOSUCHSYM` |
| `rest_ticker.json` | `GET /v2/ticker/tBTCUSD` |
| `rest_conf_pair.json` | `GET /v2/conf/pub:info:pair`, trimmed to `BTCUSD`, `ETHUSD`, `AAVE:USD` and `TESTBTC:TESTUSD`, nesting kept |
| `rest_conf_pair_futures.json` | `GET /v2/conf/pub:info:pair:futures`, trimmed to `BTCF0:USTF0`, `ETHF0:USTF0`, `AUSTRALIA200IXF0:USTF0` and `TESTBTCF0:TESTUSDTF0`, nesting kept |

## Private

Authenticated REST is on `api.bitfinex.com`, every call a POST with a JSON
body. The private websocket is `api-pub.bitfinex.com/ws/2` after an `auth`
event.

Captured unauthenticated on 2026-08-26:

| file | source |
| --- | --- |
| `rest_auth_error.json` | `POST api.bitfinex.com/v2/auth/r/orders` with no headers (HTTP 500) |

The rest are built from the field maps in Bitfinex's SDK,
`bfx-api-node-models/lib/{order,trade,position,notification}.js`. Every index
is the model's; symbols, prices and sizes were picked from pairs that
`rest_conf_pair.json` lists.

| file | shape |
| --- | --- |
| `ws_auth_ok.json`, `ws_auth_failed.json` | the `auth` acknowledgement |
| `ws_order_new.json` | `[0,"on",[...]]`, an ACTIVE spot buy |
| `ws_order_executed.json` | `[0,"oc",[...]]` at `EXECUTED @ 63951.0(-0.1)` |
| `ws_order_canceled.json` | the same order at `CANCELED` |
| `ws_order_postonly_canceled.json` | a perpetual at `POSTONLY CANCELED`, flags 4096 |
| `ws_orders_snapshot.json` | `[0,"os",[[...],[...],[...]]]` across both markets |
| `ws_exec_te.json`, `ws_exec_tu.json` | one fill as `te` (FEE null) and as `tu` (fee `-0.0639`) |
| `ws_positions_snapshot.json` | `[0,"ps",[...]]`, one ACTIVE short and one CLOSED row |
| `ws_notification_error.json` | `[0,"n",[...]]`, a refused submit |
| `rest_order_submit.json`, `rest_order_error.json` | the notification a write answers with, at SUCCESS and at ERROR |
| `rest_order_cancel.json` | the `oc-req` notification |
| `rest_orders.json`, `rest_trades.json`, `rest_positions.json` | the same rows bare, without the socket envelope; `rest_positions.json` includes a margin position on `tBTCUSD` |

The signature tests use no fixture. HMAC-SHA384 is checked against RFC 4231
test case 1, and the message shapes follow Bitfinex's SDK
(`bfx-api-node-util/lib/gen_auth_sig.js`, `bfx-api-node-rest/lib/rest2.js`):
`/api` + path + nonce + body for REST, `AUTH` + nonce + nonce for the socket.
