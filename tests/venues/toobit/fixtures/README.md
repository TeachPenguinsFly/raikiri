# Toobit fixtures

Market data captured live without keys on 2026-10-02 around 18:44 UTC from
`https://api.toobit.com` and `wss://stream.toobit.com/quote/ws/v1`.

| file | source |
|---|---|
| `rest_exchange_info.json` | `GET /api/v1/exchangeInfo`: spot BTCUSDT, ETHUSDT, PEPEUSDT, ELGUSDT (`API_TRADE_FORBIDDEN`), GAIAUSDT (`ONLINE`); contracts BTC-SWAP-USDT, ETH-SWAP-USDT, BTC-SWAP-USDC, XAUT-SWAP-USDT; `riskLimits` cut to two rows, `coins` emptied |
| `rest_funding_rate.json` | `GET /api/v1/futures/fundingRate`: BTC, ETH (8H), AXS (4H) and one `TBV_` row the catalogue does not list |
| `rest_contract_ticker.json` | `GET /quote/v1/contract/ticker/24hr`: BTC and ETH rows |
| `rest_index.json` | `GET /quote/v1/index` on 2026-10-06: the BTCUSDT, ETHUSDT, TSLAUSDT and SPELLUSDT entries of both maps |
| `rest_error.json` | `GET /api/v1/futures/positions` with a bad key: HTTP 400 |
| `ws_bbo.json`, `ws_bbo_spot.json`, `ws_bbo_ack.json` | `bookTicker` on BTC-SWAP-USDT and ETHUSDT, and the empty reply to the subscribe |
| `ws_depth_snapshot.json`, `ws_depth_snapshot_spot.json` | `diffDepth` reply to the subscribe (`f:true`, the whole book) |
| `ws_depth_delta.json`, `ws_depth_delta_next.json` | the next two `diffDepth` frames on BTC-SWAP-USDT |
| `ws_trade.json`, `ws_trade_snapshot.json` | `trade`: one live print, and the sixty recent prints sent on subscribe |
| `ws_mark.json` | `markPrice` on BTC-SWAP-USDT |
| `ws_error.json`, `ws_pong.json` | the answer to a subscribe naming an unknown symbol, and to `{"ping":<ms>}` |

The private side needs a key, so its fixtures are the documented examples with
the comments removed, from
`https://api-docs.toobit.com/api/usdt-m-account-and-trading.html` and
`https://api-docs.toobit.com/api/usdt-m-websocket-account.html`, read on
2026-10-02: `rest_order.json`, `rest_open_orders.json`, `rest_positions.json`,
`rest_balance.json`, `rest_user_trades.json`, `rest_listen_key.json`,
`user_execution.json`, `user_listen_key_expiring.json`. The signer test uses
the HMAC examples from `https://api-docs.toobit.com/api/usdt-m-example.html`.
