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
