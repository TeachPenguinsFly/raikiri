# HTX fixtures

Captured 2026-08-11 against production (`api.hbdm.com`, no credentials). Every
file is a real response or a real frame; nothing here is hand-written.

## How the websocket captures were taken

HTX gzips every websocket frame under a binary opcode, so websocat's default
stdout would mangle them. All captures used `websocat --base64`, driven by a
small script that decoded each line, inflated it, and answered the server's
ping with the exact timestamp it was given — which is what kept the sockets
alive long enough to capture anything past the first five seconds.

The `.json` files are the INFLATED text of one frame, spliced out verbatim and
never re-serialized through a JSON library: a round trip rewrites
`1.00000000E-10` as `1e-10` and `0.001000000000000000` as `0.001`, and the
spellings are exactly what several tests exist to pin.

## Raw compressed frames

| file | block type | inflates to |
| --- | --- | --- |
| `ws_bbo.frame.gz` | dynamic Huffman | `ws_bbo.json` |
| `ws_ping.frame.gz` | fixed Huffman | `ws_ping.json` |

Both are the bytes off the socket, gzip header and CRC trailer included. Two
block types on purpose: a frame carrying quotes is large enough for HTX's
encoder to build a dynamic Huffman table, while the 22-byte keepalive is not,
and an inflater that only ever saw one of them would be half tested.

## Market socket — `wss://api.hbdm.com/linear-swap-ws`

Subscribed with `{"sub":"market.BTC-USDT.bbo","id":"id1"}` and friends.

| file | topic / provenance |
| --- | --- |
| `ws_bbo.json` | `market.BTC-USDT.bbo` |
| `ws_trade.json` | `market.BTC-USDT.trade.detail`, one print |
| `ws_trades_batch.json` | `market.ETH-USDT.trade.detail`, four prints off one match |
| `ws_depth_step0.json` | `market.BTC-USDT.depth.step0`, **trimmed** (see below) |
| `ws_subscribe_ack.json` | the answer to the bbo subscribe |
| `ws_error.json` | the answer to `{"sub":"market.BTC-USDT.NOSUCH"}` |
| `ws_ping.json` | the server's keepalive; the client answers `{"pong":1786431113951}` |

`ws_depth_step0.json` is the only edited file. The live frame carried about
150 levels a side; each side was cut to its first five by splicing at the
bracket boundaries, so every number is still spelled the way HTX spelled it.

## Notification socket — `wss://api.hbdm.com/linear-swap-notification`

Subscribed with `{"op":"sub","topic":"public.BTC-USDT.funding_rate","cid":"c1"}`.
This is the only funding push HTX has: `market.{code}.funding_rate` is refused
with "invalid topic" on both the market socket and `/ws_index`.

| file | provenance |
| --- | --- |
| `ws_notify_funding.json` | a funding notify, pushed every ~5 s per contract |
| `ws_notify_sub_ack.json` | the subscribe answer, `err-code` 0 |
| `ws_notify_error.json` | the answer for `public.NOSUCH-USDT.funding_rate` |
| `ws_notify_ping.json` | the server's keepalive; answered `{"op":"pong","ts":"1786431124111"}` |

Note the keepalive spelling: this socket quotes the timestamp and the market
socket does not, which is why the adapter echoes the raw token rather than a
reformatted number.

## REST

| file | request |
| --- | --- |
| `rest_contracts.json` | `GET /linear-swap-api/v1/swap_contract_info`, **trimmed to 5 of 276 rows** |
| `rest_server_time.json` | `GET /api/v1/timestamp` |
| `rest_error_bad_contract.json` | `GET /linear-swap-api/v1/swap_contract_info?contract_code=NOSUCH-USDT` |
| `rest_error_invalid_parameter.json` | `GET /linear-swap-ex/market/depth?contract_code=BTC-USDT&type=stepXX` |
| `rest_error_not_found.json` | `GET /linear-swap-api/v1/no_such_endpoint` (HTTP 404) |

The two error files are not redundant. HTX serves market data from two
endpoint families that spell failure differently: `linear-swap-api` answers a
NUMERIC `err_code`, `linear-swap-ex` a hyphenated `err-code`, and an unrouted
path answers neither — a Spring body whose `status` is the number 404 rather
than the string "error".

`rest_contracts.json` keeps whole rows spliced out of the live response:

| row | why it is here |
| --- | --- |
| `BTC-USDT` | the ordinary case; `contract_size` 0.001, `price_tick` 0.1, 8 h funding |
| `ETH-USDT` | a second ordinary row, used by the trade-batch mapping tests |
| `SHIB-USDT` | `price_tick` `1.000000000E-9` — scientific notation the mapper must ACCEPT, exactly at FixedE9's finest |
| `PEPE-USDT` | `price_tick` `1.00000000E-10` — must be REFUSED; representing it needs a scale the domain does not have |
| `CYBER-USDT` | `contract_status` 3 with perfectly usable numbers — must be REFUSED on status alone |
