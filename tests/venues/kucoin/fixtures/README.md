# KuCoin fixtures

Captured from production with no credentials; every endpoint used here is
unauthenticated, including the websocket handshake. Files are compact
single-line JSON exactly as they came off the wire. Futures REST is
`https://api-futures.kucoin.com`, spot REST is `https://api.kucoin.com`.

REST with `curl`. The futures socket with `websocat`, subscribing and then
listening:

```
tok=$(curl -s -X POST https://api-futures.kucoin.com/api/v1/bullet-public | jq -r .data.token)
{ echo '<subscribe json>'; sleep 12; } \
  | websocat -B 200000 "wss://ws-api-futures.kucoin.com/?token=$tok&connectId=raikiri-cap" \
  | head -20 > capture.jsonl
```

## Futures, captured 2026-08-10

| file | source |
| --- | --- |
| `ws_welcome.json` | the first frame on any connection |
| `ws_ack.json` | the answer to a `subscribe` sent with `"response":true` |
| `ws_pong.json` | the answer to `{"id":"14","type":"ping"}` |
| `ws_error_topic.json` | a subscribe to `/contractMarket/nosuchtopic:XBTUSDTM` |
| `ws_error_token.json` | connecting with `?token=deadbeef` |
| `ws_ticker_v2.json` | `/contractMarket/tickerV2:XBTUSDTM` |
| `ws_execution.json` | `/contractMarket/execution:XBTUSDTM` |
| `ws_level2.json` | `/contractMarket/level2:XBTUSDTM` |
| `ws_mark_index_price.json` | `/contract/instrument:XBTUSDTM`, subject `mark.index.price` |
| `ws_funding_rate.json` | the same topic, subject `funding.rate`, pushed once a minute |
| `rest_bullet_public.json` | `POST /api/v1/bullet-public` (the token has expired) |
| `rest_timestamp.json` | `GET /api/v1/timestamp` |
| `rest_funding_rate_current.json` | `GET /api/v1/funding-rate/XBTUSDTM/current` |
| `rest_error_unknown_symbol.json` | `GET /api/v1/contracts/NOSUCHSYM` |
| `rest_error_unsupported.json` | `GET /api/v1/funding-rate/NOSUCHSYM/current` |
| `rest_contracts_active.json` | `GET /api/v1/contracts/active`, trimmed to XBTUSDTM, ETHUSDTM, XBTMU26 and PEPEUSDTM, each row byte-for-byte |
| `rest_contracts_inverse_perp.json` | the XBTUSDM row of the same endpoint; capture date not recorded |

## Futures book seed, captured 2026-08-17

One session. Subscribe first and fetch the snapshot a few seconds later; a
snapshot taken alongside a fresh subscription comes back behind the first
streamed frame.

| file | source |
| --- | --- |
| `rest_level2_snapshot.json` | `GET /api/v1/level2/snapshot?symbol=XBTUSDTM`, whole, `sequence` 1746606775926 |
| `ws_level2_change.json` | the frame holding `sequence + 1` |
| `ws_level2_change_next.json` | `sequence + 2` |
| `ws_level2_delete.json` | `sequence + 8`, `"64093.2,sell,0"` |

## Unified (UTA) feed, `x-push-futures.kucoin.com`

Captured on XBTUSDTM; dates come from the frames' own timestamps.

| file | source |
| --- | --- |
| `ws_uta_subscribe_ack.json` | the subscribe ack, 2026-09-19 |
| `ws_uta_book_snapshot.json`, `ws_uta_book_delta.json`, `ws_uta_book_delta_next.json` | `obu.FUTURES` at `increment@10ms`: the snapshot and two deltas, 2026-09-19 |
| `ws_uta_trade.json` | `trade.FUTURES`, one print, 2026-09-26 |

## Spot

| file | source |
| --- | --- |
| `rest_spot_symbols.json` | `GET https://api.kucoin.com/api/v2/symbols`, 2026-08-25, trimmed to BTC-USDT, ETH-USDT, DEBIT-USDT and WIN-BTC, each row byte-for-byte |
| `rest_spot_all_tickers.json` | `GET https://api.kucoin.com/api/v1/market/allTickers`, trimmed, 2026-09-25 (from its `time`) |
| `ws_spot_level1.json` | `/spotMarket/level1:BTC-USDT`, 2026-10-05 |
| `ws_spot_match.json` | `/market/match:BTC-USDT`, 2026-08-27 |
| `ws_spot_level2depth.json` | `/spotMarket/level2Depth50:BTC-USDT`, 2026-08-27 |

## Assembled

`rest_contracts_paused.json` is the live `ETHUSDTM` row with `"status":"Open"`
changed to `"status":"Pause"`; every contract was `Open` on the capture date.

The order-flow files below need credentials, so each is the venue's documented
example for that endpoint or channel, with two edits: `clientOid` is the ULID
`01J8ZQK9V7M3XPB2R6TDNAWC5E` (`...5F` for a second order), and symbols are
`BTC-USDT`, `ETH-USDT`, `XBTUSDTM` or `ETHUSDTM`.

| file | documented source |
| --- | --- |
| `ws_spot_order_open.json` | `/spotMarket/tradeOrders`, subject `orderChange`, `type` `open` |
| `ws_spot_order_match.json` | the same channel, `type` `match` |
| `ws_futures_order_match.json` | `/contractMarket/tradeOrders`, `type` `match`; the field values are a real recorded frame |
| `ws_futures_order_symbol_match.json` | the per-symbol form of the same topic, subject `symbolOrderChange` |
| `ws_futures_order_filled.json` | `type` `filled` |
| `ws_futures_order_canceled.json` | `type` `canceled` |
| `rest_order_ack.json` | `POST /api/v1/hf/orders` success body |
| `rest_error_cancel_gone_spot.json` | spot cancel of an order already gone (`400100`) |
| `rest_error_cancel_gone_futures.json` | the futures form of the same condition (`100004`) |
| `rest_spot_active_symbols.json` | `GET /api/v1/hf/orders/active/symbols` |
| `rest_spot_orders_active.json` | `GET /api/v1/hf/orders/active?symbol=BTC-USDT` |
| `rest_futures_orders_active.json` | `GET /api/v1/orders?status=active` |
| `rest_futures_fills.json` | `GET /api/v1/fills` |
| `rest_spot_fills.json` | `GET /api/v1/hf/fills` |
| `rest_positions.json` | `GET /api/v1/positions` |
| `rest_account_overview.json` | `GET /api/v1/account-overview?currency=USDT` |
| `rest_funding_history.json` | `GET /api/v1/funding-history?symbol=XBTUSDTM`, verbatim |
| `rest_history_positions.json` | `GET /api/v1/history-positions`, verbatim |

The funding files have no live capture behind them either: there are no keys.
`rest_funding_history.json` is the example on
`https://www.kucoin.com/docs-new/rest/futures-trading/funding-fees/get-private-funding-history`
as published on 2026-10-04. `rest_funding_history_last.json` is assembled from
it as a last page (`hasMore` false): the same row again, as overlapping pages
return it, a received charge written by hand, and a row whose
`settleCurrency` is not the contract's.

## Pro order socket, `wss://wsapi.kucoin.com/v1/private`

No order was placed to get these. They come from the docs as published on
2026-10-03: `https://www.kucoin.com/docs-new/3470252w0` (add order, with the
url, signature, session, ping and error sections) and
`https://www.kucoin.com/docs-new/3470253w0` (cancel order). The docs use hex
ids, so a test swaps in a numeric id.

| file | source |
|---|---|
| `ws_trade_session.json` | doc example: the session frame the server sends on connect |
| `ws_trade_welcome.json` | doc example: the welcome after the signed session, with the `// milliseconds` comments dropped |
| `ws_trade_connection_error.json` | doc example: the connection error |
| `ws_trade_futures_order.json` | doc example: the classic futures `futures.order` reply |
| `ws_trade_spot_order.json` | doc example: the classic spot `spot.order` reply |
| `ws_trade_futures_cancel.json` | doc example: the `futures.cancel` reply by clientOid |
| `ws_trade_spot_cancel.json` | doc example: the `spot.cancel` reply by clientOid |
| `ws_trade_order_error.json` | doc example: the place order error |
| `ws_trade_rate_limit.json` | doc example: the rate limit error |
| `ws_trade_pong.json` | assembled. The docs give the pong with `server_timestamp` as a placeholder, so this puts the session's timestamp there |
