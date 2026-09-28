# Bitunix fixtures

Captured live from `fapi.bitunix.com`, `openapi.bitunix.com` and
`wss://fapi.bitunix.com/public/` on 2026-10-02 around 18:03 UTC, without keys.
Rows are kept verbatim; long lists are cut to the rows a test names.

| file | source |
|---|---|
| `rest_trading_pairs.json` | `GET /api/v1/futures/market/trading_pairs`: BTCUSDT, ETHUSDT, SOLUSDT, BTCUSDC, BTCUSD (coin-margined), QQQUSDT (`isApiSupported:false`), FDUSDUSDT (`PREVIEW`) |
| `rest_funding_batch.json` | `GET /api/v1/futures/market/funding_rate/batch`: BTC, ETH, SOL and MILKUSDT, a row the catalogue does not list |
| `rest_tickers.json` | `GET /api/v1/futures/market/tickers`: BTC, ETH, SOL |
| `rest_rate_limited.json` | `GET /api/v1/futures/market/depth` with 40 requests in parallel: HTTP 200 and `code:100009` |
| `spot_rest_coin_pairs.json` | `GET /api/spot/v1/common/coin_pair/list`: btcusdt, ethusdt and ethbtc (`isOpen:0`) |
| `spot_rest_depth.json` | `GET /api/spot/v1/market/depth?symbol=BTCUSDT&precision=0.01` |
| `ws_connect.json` | the frame the perp socket sends on open |
| `ws_pong.json` | the answer to `{"op":"ping","ping":<unix s>}` |
| `ws_depth_book1.json`, `ws_depth_book15.json` | `depth_book1` and `depth_book15`, BTCUSDT |
| `ws_trade.json` | `trade`, BTCUSDT |
