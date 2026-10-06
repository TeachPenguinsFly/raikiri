# HTX fixtures

Captured 2026-08-11 from production (`api.hbdm.com`, no credentials) unless a
row gives another date. Every file is a real response or frame except
`rest_swap_financial_record.json`, which is built from the API reference.

HTX gzips every websocket frame under a binary opcode, and the server pings
with a timestamp that must be echoed back or the socket closes within seconds.
The captures used `websocat --base64` with a small script that decoded and
inflated each line and answered each ping. The `.json` files are the inflated
text of one frame, spliced out verbatim. Do not re-serialize them through a
JSON library: that rewrites `1.00000000E-10` as `1e-10` and
`0.001000000000000000` as `0.001`.

## Raw compressed frames

The bytes off the socket, gzip header and CRC trailer included.

| file | block type | inflates to |
| --- | --- | --- |
| `ws_bbo.frame.gz` | dynamic Huffman | `ws_bbo.json` |
| `ws_ping.frame.gz` | fixed Huffman | `ws_ping.json` |

## Market socket, `wss://api.hbdm.com/linear-swap-ws`

Subscribe with `{"sub":"market.BTC-USDT.bbo","id":"id1"}` and the like.

| file | topic |
| --- | --- |
| `ws_bbo.json` | `market.BTC-USDT.bbo` |
| `ws_trade.json` | `market.BTC-USDT.trade.detail`, one print |
| `ws_trades_batch.json` | `market.ETH-USDT.trade.detail`, four prints off one match |
| `ws_depth_step0.json` | `market.BTC-USDT.depth.step0`, each side cut to its first five levels at the bracket boundaries |
| `ws_depth_step0_full.json` | `market.BTC-USDT.depth.step0`, whole, 150 levels a side; first frame of a 15 s capture on 2026-08-17 |
| `ws_detail.json` | `market.BTC-USDT.detail`, captured 2026-09-04 |
| `ws_subscribe_ack.json` | the answer to the bbo subscribe |
| `ws_error.json` | the answer to `{"sub":"market.BTC-USDT.NOSUCH"}` |
| `ws_ping.json` | the server's keepalive; the client answers `{"pong":1786431113951}` |

## Notification socket, `wss://api.hbdm.com/linear-swap-notification`

Subscribe with `{"op":"sub","topic":"public.BTC-USDT.funding_rate","cid":"c1"}`.

| file | source |
| --- | --- |
| `ws_notify_funding.json` | a funding notify |
| `ws_notify_sub_ack.json` | the subscribe answer, `err-code` 0 |
| `ws_notify_error.json` | the answer for `public.NOSUCH-USDT.funding_rate` |
| `ws_notify_ping.json` | the server's keepalive; answered `{"op":"pong","ts":"1786431124111"}` |

## REST

| file | request |
| --- | --- |
| `rest_contracts.json` | `GET /linear-swap-api/v1/swap_contract_info`, trimmed to whole rows for `BTC-USDT`, `ETH-USDT`, `SHIB-USDT`, `PEPE-USDT` and `CYBER-USDT` |
| `rest_swap_index.json` | `GET /linear-swap-api/v1/swap_index` on 2026-10-06, trimmed from 391 rows to `BTC-USDT`, `ETH-USDT`, `BOME-USDT` and one other |
| `rest_swap_open_interest.json` | `GET /linear-swap-api/v1/swap_open_interest?business_type=swap` on 2026-10-06, trimmed from 391 rows to `BTC-USDT`, `ETH-USDT`, `SOL-USDT` and one other |
| `rest_server_time.json` | `GET /api/v1/timestamp` |
| `rest_error_bad_contract.json` | `GET /linear-swap-api/v1/swap_contract_info?contract_code=NOSUCH-USDT` |
| `rest_error_invalid_parameter.json` | `GET /linear-swap-ex/market/depth?contract_code=BTC-USDT&type=stepXX` |
| `rest_error_not_found.json` | `GET /linear-swap-api/v1/no_such_endpoint` (HTTP 404) |

## Built from the API reference

`rest_swap_financial_record.json` is the example for
`POST /linear-swap-api/v3/swap_financial_record_exact`, which needs credentials.
Field names, types and float padding are the reference's; contract codes were
changed to ones `rest_contracts.json` carries. It has five rows: the
reference's `type: 31` row with a negative `amount`, and four written here (a
`type: 30` with a positive amount, a `type: 5` opening fee, a `type: 31` with a
positive amount, and a `DOGE-USDT` row the catalogue does not carry).
