# Derive fixtures

Captured 2026-10-03 against production v2 (`api.lyra.finance`), unauthenticated:
REST with `curl` (every method is a JSON POST), the socket with `websocat` against
`wss://api.lyra.finance/ws`. Frames are verbatim, one per file. The two catalogue
bodies keep a handful of the page's rows and the page's own `pagination`; nothing
inside a kept row was retyped.

## REST, `POST https://api.lyra.finance/public/...`

| file | source |
| --- | --- |
| `rest_get_all_instruments_option.json` | `get_all_instruments` `{"instrument_type":"option","expired":false,"page_size":1000,"page":1..5}`; rows `ETH-20261009-2675-C`, `ETH-20261009-2675-P`, `XRP-20261225-4_5-P`, and the not yet activated `ETH-20271231-6000-P` |
| `rest_get_all_instruments_perp.json` | `get_all_instruments` `{"instrument_type":"perp","expired":false,"page_size":1000}`; rows `HYPE-PERP`, `ETH-PERP`, `BTC-PERP` |
| `rest_error_bad_instrument_type.json` | `get_all_instruments` `{"instrument_type":"spot"}`, refused with -32602 |

## WebSocket, `wss://api.lyra.finance/ws`

| file | source |
| --- | --- |
| `ws_subscribe_ack.json` | answer to `subscribe` for ticker_slim, orderbook and trades channels |
| `ws_subscribe_ack_refused.json` | answer to `subscribe` naming two books without the `orderbook.` prefix |
| `ws_orderbook_option.json` | `orderbook.ETH-20261009-2675-C.1.10` |
| `ws_orderbook_perp.json` | `orderbook.ETH-PERP.1.10` |
| `ws_ticker_slim_option.json` | `ticker_slim.ETH-20261009-2675-C.100` |
| `ws_ticker_slim_perp.json` | `ticker_slim.ETH-PERP.100` |
| `ws_trades_perp.json` | `trades.perp.HYPE` |
