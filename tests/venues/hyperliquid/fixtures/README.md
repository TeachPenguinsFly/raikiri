# hyperliquid fixtures

Captured from production Hyperliquid without credentials: the market-data files
on 2026-08-10, the account reads and the `/exchange` refusal later. `/info` and
the public websocket are unauthenticated, and `/info` takes a wallet address in
a plain unsigned body. Five `/exchange` replies are documented examples, listed
separately below. Trimmed files keep whole rows from the live response inside
the envelope the venue sent; no row was edited unless the table says so.

## REST, `POST https://api.hyperliquid.xyz/info`

Every read is a POST to `/info` with a JSON body naming the request type.

| file | source |
|---|---|
| `rest_meta.json` | `{"type":"meta"}`, trimmed to `BTC`, `ETH`, `MATIC` and `XRP` |
| `rest_meta_and_asset_ctxs.json` | `{"type":"metaAndAssetCtxs"}`, trimmed to the same four assets and their contexts; `marginTables` emptied |
| `rest_null_body.json` | `{"type":"l2Book","coin":"NOSUCHCOIN"}`, HTTP 200 with the body `null` |
| `rest_perp_dexs.json` | `{"type":"perpDexs"}` |
| `rest_perp_categories.json` | `{"type":"perpCategories"}` |
| `rest_meta_dex_xyz.json`, `rest_meta_dex_flx.json` | `{"type":"meta","dex":"xyz"}` and `"dex":"flx"` |
| `rest_meta_and_asset_ctxs_dex_xyz.json` | `{"type":"metaAndAssetCtxs","dex":"xyz"}` |
| `rest_meta_dex_xyz_live.json`, `rest_meta_dex_io_live.json`, `rest_meta_dex_mkts_live.json`, `rest_meta_dex_para_live.json` | `{"type":"meta","dex":…}` for `xyz`, `io`, `mkts` and `para` |

The builder-dex files (`*dex*`, `rest_perp_*`) have no recorded capture date;
`rest_meta_dex_para_live.json` carries a stamp from 2026-09-23.

## REST, account reads on `POST /info`

Taken from one real account. Each is an unsigned POST naming a public address
(written `0x…` here); no response echoes the address.

| file | source |
|---|---|
| `rest_clearinghouse_state.json` | `{"type":"clearinghouseState","user":"0x…"}` |
| `rest_spot_clearinghouse_state.json` | `{"type":"spotClearinghouseState","user":"0x…"}` |
| `rest_open_orders.json` | `{"type":"openOrders","user":"0x…"}` |
| `rest_frontend_open_orders.json` | `{"type":"frontendOpenOrders","user":"0x…"}` |
| `rest_order_status.json` | `{"type":"orderStatus","user":"0x…","oid":527278286739}` |
| `rest_order_status_unknown.json` | the same request with an oid the venue never minted |
| `rest_user_fills.json` | `{"type":"userFills","user":"0x…"}`, trimmed to five rows |
| `rest_spot_meta.json` | `{"type":"spotMeta"}`, trimmed to seven pairs and the eight tokens behind them |
| `rest_user_funding.json` | `{"type":"userFunding","user":"0x…","startTime":…}`. The `BTC` and `ETH` rows are live (2026-09-13); the `SOL` row was written by hand |

## REST, `POST https://api.hyperliquid.xyz/exchange`

`exchange_err_bad_signature.json` was captured on 2026-08-26 by posting a valid
request with a fake signature. It places nothing and needs no credentials:

```
$ curl -X POST https://api.hyperliquid.xyz/exchange -H 'Content-Type: application/json'     -d '{"action":{"type":"cancel","cancels":[{"a":0,"o":1}]},"nonce":1787746850812,
         "signature":{"r":"0x1111…","s":"0x2222…","v":27},"vaultAddress":null}'
{"status":"err","response":"Unable to recover signer."}     [HTTP 200]
```

A successful `/exchange` reply needs a real order, so these five are not captures:

| file | source |
|---|---|
| `exchange_order_resting.json` | the venue's API documentation, verbatim |
| `exchange_order_filled.json` | the venue's API documentation, verbatim |
| `exchange_order_error.json` | a real response body recorded in ccxt's Hyperliquid adapter |
| `exchange_cancel_success.json` | the venue's API documentation, verbatim |
| `exchange_cancel_missing.json` | the venue's API documentation, verbatim |

The signing tests keep their vectors inline. They come from
`hyperliquid-dex/hyperliquid-python-sdk`, `tests/signing_test.py` and
`hyperliquid/utils/signing.py` (read 2026-08-26).

## Websocket, `wss://api.hyperliquid.xyz/ws`

One session per subscription type, each read for 12–20 s. Keepalive is
`{"method":"ping"}`.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `l2Book` acknowledgement on channel `subscriptionResponse` |
| `ws_bbo.json` | first `bbo` push on `BTC` |
| `ws_trades.json` | a `trades` push carrying one print |
| `ws_trades_batch.json` | a `trades` push carrying two |
| `ws_l2book_snapshot.json`, `ws_l2book_next.json` | consecutive `l2Book` frames off one socket |
| `ws_active_asset_ctx.json` | first `activeAssetCtx` push on `BTC` |
| `ws_error.json` | the reply to a subscription of `{"type":"nosuchchannel"}`; the venue closes the socket after it |
| `ws_pong.json` | the answer to `{"method":"ping"}` |
| `ws_all_mids.json` | an `allMids` push, trimmed to four mids |
| `ws_hip3_bbo.json`, `ws_hip3_l2book.json`, `ws_hip3_trades.json`, `ws_hip3_active_asset_ctx.json` | `bbo`, `l2Book`, `trades` and `activeAssetCtx` on `xyz:XYZ100`; frame stamps are 2026-09-26 |

## WebSocket post replies

No order was placed to get these. They come from
`https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/post-requests`
and the exchange endpoint page, as published on 2026-10-03.

| file | source |
|---|---|
| `ws_post_order_resting.json` | doc example: the action post reply with a resting status. The published example has a trailing comma after `oid`, removed here |
| `ws_post_order_error.json` | assembled. The documented post envelope carries the exchange page's "minimum value of $10" error status |
| `ws_post_bad_signature.json` | assembled. The documented post envelope carries `exchange_err_bad_signature.json` |
| `ws_post_error.json` | assembled. The docs describe the `error` type as a string mirroring the http status and text but show none |
