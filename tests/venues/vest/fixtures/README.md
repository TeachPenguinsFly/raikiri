# Vest fixtures

Captured live from `server-prod.hz.vestmarkets.com`, `api-gateway.hz.vestmarkets.com` and
`wss://ws.hz.vestmarkets.com/ws` on 2026-10-06 between 15:07 and 15:12 UTC, without keys. The REST
answers keep twelve symbols: BTC, ETH, SOL, NVDA, AAPL, SPY, MSTR, DDOG (halted), NOK the stock, and
the EUR, JPY and NOK currency perps.

| file | source |
|---|---|
| `rest_exchange_info.json` | `GET /v2/exchangeInfo`, the twelve rows |
| `rest_ticker_latest.json` | `GET /v2/ticker/latest`: mark, index and the hourly funding rate |
| `rest_ticker_24hr.json` | `GET /v3/ticker/24hr` on the gateway host: volume by account kind |
| `rest_oi.json` | `GET /v3/oi` on the gateway host: open interest per side |
| `rest_ntp.json` | `GET /v2/exchangeInfo/ntp`, the clock probe |
| `ws_depth.json` | `BTC-PERP@depth`, the whole book |
| `ws_trades.json` | `BTC-PERP@trades`, one print |
| `ws_subscribe.json` | the answer to a `SUBSCRIBE` naming a market that does not exist |
| `ws_ping_ack.json`, `ws_pong.json` | the two frames that answer `{"method":"PING"}` |
