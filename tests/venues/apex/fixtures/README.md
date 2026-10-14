# apex fixtures

Market data was captured live on 2026-09-25 from `omni.apex.exchange` and
`wss://quote.omni.apex.exchange/realtime_public`, no credentials. Frames are
verbatim; the catalogue is trimmed. The private rows come from the venue's API
documentation, since no account was used; each is listed below.

| file | source |
|---|---|
| `rest_symbols.json` | `GET /api/v3/symbols`, trimmed to BTC (enabled, `UNKNOWN_CONTRACT_TYPE`), TAO (2%/h cap), IO (reduce-only), SUNDOG (disabled), one prediction row, and the stocks IWM (disabled) and XAU; re-serialised, and every value the adapter reads is a JSON string or bool |
| `ws_orderbook_snapshot.json`, `ws_orderbook_delta.json`, `ws_orderbook_delta_next.json` | `orderBook25.H.ETHUSDT`, the snapshot and the two deltas after it in one session (`u` 3063780, 81, 82) |
| `ws_trade_snapshot.json`, `ws_trade_delta.json` | `recentlyTrade.H.ETHUSDT`: the 50-print backlog sent on subscribe, then one live print |
| `ws_ticker_snapshot.json`, `ws_ticker_delta.json`, `ws_ticker_delta_mark.json` | `instrumentInfo.H.BTCUSDT`: the snapshot, a delta carrying only `symbolStatus`, and one carrying the mark |
| `ws_subscribed.json` | the acknowledgement of a three-topic subscribe |
| `ws_refused.json` | `error:handler not found` for a frame holding one valid and one unknown topic; the valid one was not served |
| `ws_refused_already_subscribed.json` | a frame repeating a held topic beside a new one; the new one was not served |
| `ws_server_ping.json` | the server's ping, sent every 15 s |
| `rest_symbols_signing.json` | `GET /api/v3/symbols` on 2026-10-14, trimmed to `assets` and the DOGE, WIF, ZRO and ENA perps, keeping only the fields an order signature reads |
| `rest_error_timestamp.json` | the live refusal of a private call without headers |
| `rest_error_credential.json` | the live refusal of a made-up api key |
| `rest_account.json` | the documented `GET /v3/account` answer |
| `rest_create_order.json` | the documented `POST /v3/order` answer, inside the live `data` and `timeCost` envelope |
| `rest_open_orders.json` | the documented `GET /v3/open-orders` answer |
| `rest_fills.json` | the documented `GET /v3/fills` page, inside the live envelope |
| `ws_account_pending.json`, `ws_account_open.json`, `ws_account_canceled.json`, `ws_account_filled.json` | the documented `ws_zk_accounts_v3` pushes for a submitted, resting, canceled and filled order |
| `ws_account_snapshot.json` | the documented subscription snapshot, inside the push envelope |
| `ws_private_ping.json` | the documented server ping |
| `ws_login_ack.json` | a login answer in the documented subscribe-answer shape |
