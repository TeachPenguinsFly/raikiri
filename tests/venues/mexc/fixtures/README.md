# mexc fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file in the two market-data sections below was **captured live** from
production MEXC futures on 2026-08-10. No credentials are involved — all of
these are public endpoints and the public websocket. Nothing is hand-written or
reshaped from docs, and no file was round-tripped through a JSON library: the
bytes are sliced verbatim out of the capture, because re-serializing turns
MEXC's `0.000085` into `8.5e-05`, which the fixed-point parser refuses.

Order entry cannot be captured without credentials. The last section says,
file by file, which of those bytes came off the wire anyway and which were
assembled from MEXC's reference.

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
| `ws_depth_full.json` | first `push.depth.full` of a `limit:50` subscribe, recaptured 2026-08-17 |
| `ws_depth_update.json`, `ws_depth_update_next.json` | the first two `push.depth` deltas |
| `ws_pong.json` | the answer to `{"method":"ping"}` |
| `ws_error.json` | `sub.ticker` on `NOSUCH_USDT` |
| `ws_idle_timeout.json` | see below |

`depth.full` takes fixed tiers only, and the refusal is loud rather than a
silent downgrade: 5, 10, 20, 50 and 200 all stream, while 60, 80, 99 and 100
each drew `{"channel":"rs.error","data":"Not support limit"}` on a fresh
socket. 50 is therefore the deepest tier that fits `k_book_levels`.

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

## Order entry (added 2026-08-26)

Order entry needs credentials and this repository has none, so the private
files below split into two provenances and each one says which it is.

### Captured live, unauthenticated

These are real refusal bodies: a request with no key, or with a made-up one,
against production. They are the evidence behind `classify` / `classify_spot`.

| file | how |
|---|---|
| `rest_exchange_info.json` | `GET https://api.mexc.com/api/v3/exchangeInfo` (2123 rows), five rows sliced VERBATIM out of the response |
| `rest_error_not_logged_in.json` | `GET contract.mexc.com/api/v1/private/order/list/open_orders/BTC_USDT` with no headers |
| `rest_error_key_expired.json` | the same call against `api.mexc.com` with `ApiKey: bogus` |
| `rest_spot_error_key.json` | `POST api.mexc.com/api/v3/order` with `X-MEXC-APIKEY: bogus` |
| `rest_spot_error_not_found.json` | `GET /api/v3/openOrders?symbol=BTCUSDT` with a key header and NO timestamp/signature |

`rest_spot_error_not_found.json` earns its place: a signed spot endpoint reached
without its timestamp and signature answers **404**, not a parameter error, so a
signing bug in this adapter would present as a routing bug.

The two futures refusals were captured against DIFFERENT hosts on purpose.
`contract.mexc.com` still answers private GETs with MEXC's own envelope but
403s a private POST at the CDN, while `api.mexc.com` answers both — which is
how the 2026-01 host migration is observable without a key, and why order entry
targets `api.mexc.com`.

`rest_exchange_info.json` is trimmed only in the sense that rows were
**selected**; each is verbatim bytes out of the live response, because
re-serializing turns `"0.000001"` into `1e-06`, which the fixed-point parser
refuses. It keeps

- `BTCUSDT` and `ETHUSDT` — loadable, `quotePrecision` 2 with
  `baseSizePrecision` `0.000001` and `0.0001`. The two precisions in one row
  arrive in DIFFERENT forms: a digit count for price, a decimal step for size;
- `WNTUSDT` — `isSpotTradingAllowed: false` with an otherwise usable pair of
  steps, so the refusal is attributable to the flag alone. 104 of the 2123 rows
  are like this;
- `XENUSDT` — `quotePrecision: 12`. The tick is 10^-scale and FixedE9 carries
  nine fractional digits, so this row must be refused rather than rounded;
- `PYTHUSDT` — `baseSizePrecision: "0"`. 1590 rows carry a zero size step,
  which is not a lot size and cannot become one.

Every one of the 2123 rows carried `status: "1"` and an `orderTypes` list of
`LIMIT`/`MARKET`/`LIMIT_MAKER` — 2088 with all three and 35 without `MARKET`.
**Not one advertised `IMMEDIATE_OR_CANCEL` or `FILL_OR_KILL`**, though the API
reference lists both as `type` values. That measurement is why an IOC or FOK
spot order is refused here instead of being downgraded to GTC.

### Built from the API reference

No credentials exist to capture these, so they are assembled from the payload
examples in MEXC's own reference and marked as such. Field NAMES and TYPES are
the doc's; the symbols and numbers were changed to ones the catalogue fixtures
above actually carry, so the mapper has real tick and lot sizes to divide by.

| file | reference shape |
|---|---|
| `ws_personal_order_filled.json` | the `push.personal.order` payload example, VERBATIM |
| `ws_personal_order_new.json`, `_partial.json`, `_cancelled.json` | the same shape at `state` 2 with `dealVol` 0, at 2 with `dealVol` 1, and at 4 |
| `ws_personal_deal.json` | the `order/deal_details` row shape (`isTaker`, not `taker`) on the inferred channel — see below |
| `ws_personal_position.json` | the `push.personal.position` payload example, VERBATIM |
| `ws_login_ack.json` | `rs.login`, the channel the docs name for a successful login |
| `rest_futures_open_orders.json` | `GET /api/v1/private/order/list/open_orders` |
| `rest_futures_order_deals.json` | `GET /api/v1/private/order/list/order_deals` (`taker`, not `isTaker`) |
| `rest_futures_positions.json` | `GET /api/v1/private/position/open_positions` |
| `rest_futures_cancel_partial.json` | the cancel response example, VERBATIM — a per-order `errorCode` 2040 beside a 0 |
| `rest_spot_order_new.json` | `POST /api/v3/order` |
| `rest_spot_open_orders.json` | `GET /api/v3/openOrders` |
| `rest_spot_my_trades.json` | `GET /api/v3/myTrades` |

`ws_personal_deal.json` is the one file whose CHANNEL NAME is inferred rather
than quoted. MEXC documents the personal filter keys as `order`, `order.deal`,
`position`, `asset` and shows payload examples on `push.personal.order`,
`push.personal.position` and `push.personal.asset` — but none for the deal
stream. `push.personal.order.deal` follows the mapping the other three
establish. It is also a strict SUFFIX extension of `push.personal.order`, which
is exactly the hazard `push.depth` / `push.depth.full` already posed here, so
the dispatch test pins that the two do not collapse.

`ws_personal_order_filled.json` keeps the doc's own `externalOid`,
`_m_95bc2b72d3784bce8f9efecbdef9fe35` — 35 characters, which is what MEXC's own
web orders carry and is not a ULID. It is the evidence that a foreign id must
leave the client-order-id field empty instead of dropping the event: an order we
did not place is still one the reconciler has to see.
