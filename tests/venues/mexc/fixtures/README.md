# mexc fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file below was **captured live** from production MEXC futures on
2026-08-10. No credentials are involved — all of these are public endpoints and
the public websocket. Nothing is hand-written or reshaped from docs, and no
file was round-tripped through a JSON library: the bytes are sliced verbatim
out of the capture, because re-serializing turns MEXC's `0.000085` into
`8.5e-05`, which the fixed-point parser refuses.

MEXC sends **uncompressed JSON text frames** — no per-message deflate, no
binary payloads — so `libs/net`'s text-only `WsClient` reads them as they are.

## Websocket — `wss://contract.mexc.com/edge`

Subscriptions are `{"method":"sub.<channel>","param":{"symbol":"BTC_USDT"}}`,
one symbol per message.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `sub.ticker` acknowledgement |
| `ws_ticker.json` | first `push.ticker` after it |
| `ws_deal.json` | a `push.deal` carrying four prints, two directions |
| `ws_depth_full.json` | first `push.depth.full` of a `limit:5` subscribe |
| `ws_depth_update.json`, `ws_depth_update_next.json` | the first two `push.depth` deltas |
| `ws_pong.json` | the answer to `{"method":"ping"}` |
| `ws_error.json` | `sub.ticker` on `NOSUCH_USDT` |
| `ws_idle_timeout.json` | see below |

The two `push.depth` files are **consecutive frames off one socket**, so the
version chain across them is real rather than constructed:
`update.end + 1 == update_next.begin`. `ws_depth_update.json` also happens to
lead its `bids` with a level 1900 below the touch, which is the evidence that a
delta is a set of CHANGED levels in no order — the reason `map_bbo` refuses
anything but a snapshot.

`ws_idle_timeout.json` was captured by holding a socket open for 100 s while
sending nothing. It kept receiving `push.ticker` the whole time and was still
closed at 62 s with `more than 60 seconds no response, close the channel`.
Inbound traffic does not reset MEXC's timer; only client messages do. That
frame arrives on `rs.error`, the same channel a refused subscription uses, and
carries no code to tell the two apart.

## Not captured

`sub.funding.rate` exists and answers `{"channel":"rs.sub.funding.rate",
"data":"success"}`, then pushes **nothing** — 90 s on BTC_USDT produced no
frame, so it appears to fire only at settlement. Funding therefore rides on
`push.ticker`, which carries `fundingRate` and `fairPrice` every ~3 s but no
settlement time; `rest_funding_rate.json` is where the schedule comes from.

An unknown `method` gets **no answer at all** (`sub.bogus` with a valid symbol
drew nothing while two malformed `sub.ticker` calls each drew an `rs.error`),
so there is no "unknown channel" fixture to capture and silence is not evidence
a subscription was accepted.

## REST — `https://contract.mexc.com`

| file | source |
|---|---|
| `rest_contract_detail.json` | `GET /api/v1/contract/detail` (1086 rows), trimmed to four |
| `rest_server_time.json` | `GET /api/v1/contract/ping` |
| `rest_depth.json` | `GET /api/v1/contract/depth/BTC_USDT?limit=5` |
| `rest_ticker.json` | `GET /api/v1/contract/ticker?symbol=BTC_USDT` |
| `rest_funding_rate.json` | `GET /api/v1/contract/funding_rate/BTC_USDT` |
| `rest_error_bad_contract.json` | the same ticker call with `symbol=NOSUCH_USDT` — a real `1001` |
| `rest_error_bad_parameter.json` | the depth call with `limit=abc` — a real `600` |

`rest_depth.json` earns its place twice: it is a snapshot with no `channel`
field at all, and its `cts` is `null`, so it is the only fixture that exercises
the fallback to the envelope's `timestamp`.

`rest_contract_detail.json` is the only trimmed file, and only in the sense
that rows were **selected**: each of the four is a verbatim row from the live
response, re-emitted inside the envelope the venue sent. It keeps

- `BTC_USDT` and `ETH_USDT` — `state: 0`, both loadable, at `priceScale` 1 and
  2 and `contractSize` 0.0001 and 0.01. Neither contract size is 1, which is
  the whole point of not hardcoding one;
- `AIINU_USDT` — `apiAllowed: false`. Order placement is blocked on it and
  market data is not, so the loader must NOT filter on that field. 26 of the
  1086 rows are like this;
- `SATS_USDT` — `priceScale: 11`. The tick is derived as 10^-scale and FixedE9
  carries nine fractional digits, so this row must be refused rather than
  rounded. Three of 1086 rows are past nine (`PEPE_USDT` and `CHEEMS_USDT` at
  10, this one at 11), which is exactly the gap between the 1086 rows the venue
  ships and the 1083 a live gateway loads.

Every row in the live response carried `state: 0`; no disabled contract was
observable on 2026-08-10, so the "must be refused" row here is refused on
precision rather than on state. `map_instrument` still rejects a non-zero
`state` first, and a unit test pins that with a synthetic row rather than
pretending a captured one exists.
