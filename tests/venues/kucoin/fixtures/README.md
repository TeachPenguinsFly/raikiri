# KuCoin Futures fixtures

Captured 2026-08-10 (UTC) against production `api-futures.kucoin.com` /
`ws-api-futures.kucoin.com`. No credentials are involved: every endpoint used
here is unauthenticated, including the websocket handshake.

REST with `curl`. Websocket with `websocat`, subscribing and then listening:

```
tok=$(curl -s -X POST https://api-futures.kucoin.com/api/v1/bullet-public | jq -r .data.token)
{ echo '<subscribe json>'; sleep 12; } \
  | websocat -B 200000 "wss://ws-api-futures.kucoin.com/?token=$tok&connectId=raikiri-cap" \
  | head -20 > capture.jsonl
```

Every file is compact single-line JSON, exactly as it came off the wire —
including Java's double formatting (`1.0E-10`, `64174.10`), which the parsers
must survive.

## Captured live

| File | Source |
| --- | --- |
| `ws_welcome.json` | first frame on any connection; echoes our `connectId` |
| `ws_ack.json` | answer to a `subscribe` sent with `"response":true` |
| `ws_pong.json` | answer to `{"id":"14","type":"ping"}` — note the timestamp is MICROseconds |
| `ws_error_topic.json` | subscribing to `/contractMarket/nosuchtopic:XBTUSDTM` |
| `ws_error_token.json` | connecting with `?token=deadbeef` — arrives in-band on an open socket, not as an HTTP 401 |
| `ws_ticker_v2.json` | `/contractMarket/tickerV2:XBTUSDTM` |
| `ws_execution.json` | `/contractMarket/execution:XBTUSDTM` |
| `ws_level2.json` | `/contractMarket/level2:XBTUSDTM` |
| `ws_mark_index_price.json` | `/contract/instrument:XBTUSDTM`, subject `mark.index.price` |
| `ws_funding_rate.json` | same topic, subject `funding.rate` — pushed once a minute, so a 75-frame capture caught exactly one |
| `rest_bullet_public.json` | `POST /api/v1/bullet-public`; the token is long expired |
| `rest_timestamp.json` | `GET /api/v1/timestamp` |
| `rest_funding_rate_current.json` | `GET /api/v1/funding-rate/XBTUSDTM/current` |
| `rest_error_unknown_symbol.json` | `GET /api/v1/contracts/NOSUCHSYM` |
| `rest_error_unsupported.json` | `GET /api/v1/funding-rate/NOSUCHSYM/current` |
| `rest_contracts_active.json` | `GET /api/v1/contracts/active`, trimmed from 677 rows to 4, each row byte-for-byte as served |

Captured 2026-08-17 in one session, so that the seed and the changes belong to
the same book. The websocket was subscribed first and the snapshot fetched six
seconds later, which is the ONLY ordering that works: a snapshot taken
alongside a fresh subscription came back 1555 sequences behind the first
streamed frame, and the chain being contiguous, those changes are gone.

| File | Source |
| --- | --- |
| `rest_level2_snapshot.json` | `GET /api/v1/level2/snapshot?symbol=XBTUSDTM`, whole — 1000 levels a side, `sequence` 1746606775926 |
| `ws_level2_change.json` | the frame holding `sequence + 1`, a new bid at 62167.6 — 871 levels down but still inside the seed's own range |
| `ws_level2_change_next.json` | `sequence + 2`, a bid at 0.1: real, and six thousand levels below anything the seed vouched for |
| `ws_level2_delete.json` | `sequence + 8`, `"64093.2,sell,0"` — the size of zero that removes ask level 5 |

The four kept contract rows are the ones the mapper has to disagree about:

- `XBTUSDTM` — accepted. `multiplier` 0.001, so one contract is 0.001 XBT.
- `ETHUSDTM` — accepted, a different tick/multiplier pair.
- `XBTMU26` — refused: `type` is `FFICSX`, a dated future sharing the endpoint
  with the perpetuals, and its `multiplier` is `-1.0` because it is inverse.
- `PEPEUSDTM` — refused: `tickSize` is `1.0E-10`, one decade finer than FixedE9
  models. Refusing beats rounding a tick size to zero.

## Assembled

| File | Why |
| --- | --- |
| `rest_contracts_paused.json` | The live `ETHUSDTM` row with `"status":"Open"` changed to `"status":"Pause"`. All 677 contracts were `Open` on the capture date, so there was no real non-Open row to keep, and the mapper's status gate would otherwise go untested. |

## Spot catalogue, captured live

`rest_spot_symbols.json` — `GET https://api.kucoin.com/api/v2/symbols` (2026-08-25,
no credentials), trimmed from 1003 rows to four, each byte-for-byte as served.
Note the HOST: this is the only fixture here from `api.kucoin.com` rather than
`api-futures.kucoin.com`, and it is the reason spot order entry can name a
symbol at all.

The four rows are the ones the mapper has to disagree about:

- `BTC-USDT` — accepted. `priceIncrement` 0.1, `baseIncrement` 0.00000001.
- `ETH-USDT` — accepted, a finer tick and a coarser lot.
- `DEBIT-USDT` — refused: `enableTrading` is false. It was the ONLY suspended
  pair of the 1003 on the capture date.
- `WIN-BTC` — refused: `priceIncrement` is `0.000000000001`, three decades below
  what FixedE9 models. Refusing beats rounding a tick size to zero.

## Order flow, assembled

No fixture below was captured: order entry needs credentials, and this
repository has none. Each is the venue's own documented example for that
endpoint or channel, so it proves the parsers read the shape KuCoin publishes —
not that production sends exactly these bytes. Replace them with captures the
first time a funded key runs against the venue.

Two edits were made across all of them, and only these two:

- `clientOid` is a 26-char Crockford ULID (`01J8ZQK9V7M3XPB2R6TDNAWC5E`, and
  `...5F` for a second order), because that is what raikiri mints. KuCoin's own
  examples use UUIDs, which are 36 characters and do not fit
  `ExchangeClientOrderId` — the mapper drops those, which is a property the
  tests pin separately rather than something the shared fixtures should hide.
- Symbols are `BTC-USDT` / `ETH-USDT` / `XBTUSDTM` / `ETHUSDTM`, so the rows
  resolve against the two catalogue fixtures above.

| File | Documented source |
| --- | --- |
| `ws_spot_order_open.json` | `/spotMarket/tradeOrders`, subject `orderChange`, `type` `open` |
| `ws_spot_order_match.json` | same channel, `type` `match` — adds `matchPrice`, `matchSize`, `liquidity`, `feeType`, `tradeId` |
| `ws_futures_order_match.json` | `/contractMarket/tradeOrders`, `type` `match`; field values are a real recorded frame (ETHUSDTM, `price` `"0"` on a market order) |
| `ws_futures_order_symbol_match.json` | the PER-SYMBOL form of the same topic, whose subject is `symbolOrderChange` and which names neither `orderId` nor `clientOid` |
| `ws_futures_order_filled.json` | `type` `filled` — the venue closing an order whose trades already arrived as `match` |
| `ws_futures_order_canceled.json` | `type` `canceled` |
| `rest_order_ack.json` | `POST /api/v1/hf/orders` success body |
| `rest_error_cancel_gone_spot.json` | spot cancel of an order that is already gone: a generic `400100` whose TEXT is the only discriminator |
| `rest_error_cancel_gone_futures.json` | the futures spelling of the same condition, `100004` |
| `rest_spot_active_symbols.json` | `GET /api/v1/hf/orders/active/symbols` |
| `rest_spot_orders_active.json` | `GET /api/v1/hf/orders/active?symbol=BTC-USDT`; `data` is the array itself, and the second row is partially filled |
| `rest_futures_orders_active.json` | `GET /api/v1/orders?status=active`; `data` is a PAGE object wrapping `items`, `size`/`dealSize` are JSON numbers, and there is no `remainSize` |
| `rest_futures_fills.json` | `GET /api/v1/fills` |
| `rest_spot_fills.json` | `GET /api/v1/hf/fills` — same `items` shape, but `tradeId` is a JSON number where futures sends a string |
| `rest_positions.json` | `GET /api/v1/positions`; the ETHUSDTM row is short (`currentQty` -40) and its `avgEntryPrice` 2068.555 is NOT a multiple of the 0.01 tick, which is normal for an average and must not discard the row |
