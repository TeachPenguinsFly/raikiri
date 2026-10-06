# Polymarket Perps fixtures

Captured live from `api.perpetuals.polymarket.com` and `wss://ws.perpetuals.polymarket.com/v1/ws`
on 2026-10-06 between 15:11 and 15:12 UTC, without keys. The REST answers keep five instruments:
SP500 (1), GOLD (2), WTIOIL (3), BTC (6) and TSLA (21).

| file | source |
|---|---|
| `rest_instruments.json` | `GET /v1/info/instruments`, the five rows |
| `rest_bbo.json` | `GET /v1/info/bbo`, the seed for a market whose `bbo::` channel is quiet |
| `rest_time.json` | `GET /v1/info/time`, the clock probe |
| `ws_bbo.json` | `bbo::6` |
| `ws_book.json` | `book::21`, the whole ladder |
| `ws_trades.json` | `trades::6` |
| `ws_tickers.json` | `tickers::all`, the TSLA row |
| `ws_statistics.json` | `statistics::all`, the SP500 row with its hourly candles |
| `ws_answer.json` | the answer to a `sub` whose last channel, `nope::6`, does not exist |
| `ws_pong.json` | the answer to `{"req":"post","op":{"type":"ping"}}` |
