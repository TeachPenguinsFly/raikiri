# XT futures fixtures

Every file here was captured from **production XT on 2026-08-10/11**. Nothing
is hand-written or synthesised. Two files are edited captures and say so below;
the rest are verbatim response bodies and websocket frames, reformatted only by
being written as one compact line.

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
| `ws_depth.json` | `depth@btc_usdt,5,100ms`. The BBO source: a self-contained top-N snapshot with sizes. |
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
