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

## Options, captured live on 2026-10-03

From production with no credentials. The REST page is trimmed to four rows of
its `list` and written back without whitespace; the fields are as served. The
websocket files are single
frames copied verbatim from one session on
`wss://stream.bybit.com/v5/public/option` that subscribed
`tickers.BTC-26MAR27-90000-C-USDT`, `orderbook.25.BTC-26MAR27-90000-C-USDT`
and `publicTrade.BTC`, read for 16 s.

| file | source |
|---|---|
| `rest_instruments_option.json` | `GET https://api.bybit.com/v5/market/instruments-info?category=option&baseCoin=BTC&limit=1000`, `list` cut to the 90000 call and put of 26MAR27 and the 106000 pair of 25JUN27 |
| `option_tickers.json` | first `tickers` push: a full snapshot with `bidIv`, `askIv`, `markPriceIv` and the greeks |
| `option_orderbook_snapshot.json` | the `orderbook.25` snapshot |
| `option_public_trade.json` | a `publicTrade.BTC` push, which carries every BTC option's prints |
| `option_subscribe_ack.json` | the option socket's `COMMAND_RESP` acknowledgement with `successTopics` and `failTopics` |

## Spot tickers, captured live on 2026-10-06

| file | source |
|---|---|
| `rest_tickers_spot.json` | `GET https://api.bybit.com/v5/market/tickers?category=spot`, `list` cut from 530 rows to BTCUSDT, ETHUSDT, SOLUSDT, DOGEUSDT and one other, written back without whitespace |
