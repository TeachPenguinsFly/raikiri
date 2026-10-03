# bybit fixtures

This file lists only the v5 trade socket replies. No order was placed to get
them. They come from
`https://bybit-exchange.github.io/docs/v5/websocket/trade/guideline`, as
published on 2026-10-03. The docs use the reqId `test-005`, so a test swaps in
a numeric id.

| file | source |
|---|---|
| `ws_trade_auth.json` | doc example: the `auth` success reply |
| `ws_trade_create.json` | doc example: the `order.create` success reply |
| `ws_trade_pong.json` | doc example: the reply to `{"op":"ping"}` on the trade socket |
| `ws_trade_auth_failure.json` | assembled. The docs give only the retCode table for auth, so this is the success reply with retCode 10004 ("invalid sign") |
| `ws_trade_error.json` | assembled. The docs give no single-order refusal, so this is the `order.create` reply carrying code 10001 with the message from the docs' batch example |
