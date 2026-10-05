# grvt fixtures

Captured from production `market-data.grvt.io` on 2026-09-24, public
endpoints, no credentials. Frames are verbatim. The socket is
`wss://market-data.grvt.io/ws/full`, and every stream below is on
ETH_USDT_Perp.

| file | source |
|---|---|
| `rest_all_instruments.json` | `POST /full/v1/all_instruments` with body `{"is_active":true}`, trimmed to four rows |
| `rest_all_instruments_classes.json` | the same call on 2026-10-05, trimmed to a crypto, an equity and a commodity row |
| `rest_error.json` | a malformed REST request, HTTP 400 |
| `ws_book_d_ack.json`, `ws_book_d_snapshot.json`, `ws_book_d_delta.json`, `ws_book_d_delta_next.json` | `v1.book.d` at `@500`, one subscription in order; the ack's `latest_sequence_number` is the first delta's `prev_sequence_number` |
| `ws_mini_d_snapshot.json`, `ws_mini_d_delta.json` | `v1.mini.d` at `@0` |
| `ws_ticker_s.json` | `v1.ticker.s` at `@5000` |
| `ws_trade_snapshot.json`, `ws_trade.json` | `v1.trade` at `@50`: the backlog sent on subscribe, then one live print |
| `ws_error.json` | the answer to a subscribe naming an invalid instrument |
