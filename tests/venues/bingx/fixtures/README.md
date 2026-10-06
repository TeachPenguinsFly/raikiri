# BingX fixtures

The public files were captured from BingX production, no credentials. REST
bodies were saved straight from `curl` against `https://open-api.bingx.com`;
only the two catalogue files were trimmed, and their surviving rows are
byte-for-byte what the venue sent.

BingX sends every socket frame gzip-compressed under a binary opcode. Capture
with `websocat --base64` (the default stdout path mangles non-UTF-8 bytes) and
inflate each message with `python3 -c "import zlib; zlib.decompress(raw, 31)"`.
The `.json` files are the inflated text; the `.frame.gz` files are the raw
bytes.

The private files are BingX's published example payloads, since no key exists
for this account. The edits to them are listed per row.

## Websocket, `wss://open-api-swap.bingx.com/swap-market`

| file | subscription |
| --- | --- |
| `ws_book_ticker.json` | `BTC-USDT@bookTicker` |
| `ws_trade.json` | `BTC-USDT@trade` |
| `ws_depth100.json` | `BTC-USDT@depth100@100ms`, recaptured 2026-08-17 |
| `ws_depth100.frame.gz` | the raw gzip frame that inflates to `ws_depth100.json` |
| `ws_ping.frame.gz` | the raw gzip frame carrying `Ping` |
| `ws_mark_price.json` | `BTC-USDT@markPrice` |
| `ws_ticker.json` | `BTC-USDT@ticker`, captured 2026-09-04 |
| `ws_subscribe_ack.json` | the ack to any subscribe |
| `ws_error.json` | `NOSUCH-USDT@bookTicker`, code 80015 |
| `ws_error_data_type.json` | `BTC-USDT@fundingRate`, code 80015 "dataType not support" |

## Websocket, `wss://open-api-ws.bingx.com/market` (spot)

Capture dates come from the frames' own timestamps.

| file | subscription |
| --- | --- |
| `ws_spot_ticker.json` | `BTC-USDT@ticker`, captured 2026-09-04 in the same minute as `ws_ticker.json` |
| `ws_spot_book_ticker.json` | `BTC-USDT@bookTicker`, 2026-08-27 |
| `ws_spot_trade.json` | `BTC-USDT@trade`, 2026-08-27 |
| `ws_spot_depth20.json` | `BTC-USDT@depth20`, 2026-08-27 |

## REST, `https://open-api.bingx.com`

| file | request |
| --- | --- |
| `rest_contracts.json` | `GET /openApi/swap/v2/quote/contracts`, trimmed to 5 rows: BTC-USDT, ETH-USDT, SOL-USDT, NCCOCOFFEE2USD-USDT and POWER-USDT (2026-09-15) |
| `rest_premium_index.json` | `GET /openApi/swap/v2/quote/premiumIndex?symbol=BTC-USDT` |
| `rest_server_time.json` | `GET /openApi/swap/v2/server/time` |
| `rest_error_bad_symbol.json` | `GET /openApi/swap/v2/quote/depth?symbol=NOSUCH-USDT&limit=5`, code 109425 |
| `rest_error_not_found.json` | `GET /openApi/swap/v2/quote/nope`, code 100400 |
| `rest_error_bad_key.json` | `POST /openApi/swap/v2/trade/order` with no key, 2026-08-26: HTTP 200 carrying 100413 |
| `rest_spot_symbols.json` | `GET /openApi/spot/v1/common/symbols`, 2026-08-26, trimmed to 5 rows: BTC-USDT, ETH-USDT, APEPE-USDT, ANSEM-USDT and M3M3-USDT |
| `rest_spot_ticker_24h.json` | `GET /openApi/spot/v1/ticker/24hr`, 2026-10-06, no parameters, trimmed to its first 5 rows of 656: BTC-USDT, ETH-USDT, USDC-USDT, XRP-USDT and BNB-USDT; numbers are bare |

## Private (BingX's published examples)

| file | source |
| --- | --- |
| `rest_open_orders_perp.json` | `GET /openApi/swap/v2/trade/openOrders`; `clientOrderId` filled in lowercase and a second `PARTIALLY_FILLED` row added |
| `rest_open_orders_spot.json` | `GET /openApi/spot/v1/trade/openOrders` |
| `rest_order_ack_perp.json` | `POST /openApi/swap/v2/trade/order` |
| `rest_order_ack_spot.json` | `POST /openApi/spot/v1/trade/order` |
| `rest_positions.json` | `GET /openApi/swap/v2/user/positions` |
| `rest_listen_key.json` | `POST /openApi/user/auth/userDataStream`, the bare `{"listenKey":…}` form from the skill reference |
| `rest_listen_key_wrapped.json` | the same answer in the usual envelope, as the API reference shows it |
| `rest_position_mode.json` | `GET /openApi/swap/v1/positionSide/dual` |
| `rest_balance.json` | `GET /openApi/swap/v3/user/balance`; the USDT row is verbatim and a USDC row was added |
| `rest_income.json` | `GET /openApi/swap/v2/user/income`; row one has its symbol swapped to one the catalogue fixtures carry, and a `TRADING_FEE` row and a `REALIZED_PNL` row were added |
| `ws_order_update_perp_new.json` | `ORDER_TRADE_UPDATE` with `x` `NEW` |
| `ws_order_update_perp_fill.json` | the same order with `x` `TRADE` |
| `ws_order_update_perp_cancel.json` | `x` `CANCELED` with `X` spelled `CANCELLED` |
| `ws_order_update_spot_fill.json` | `spot.executionReport` |
| `ws_listen_key_expired.json` | the published `listenKeyExpired` push, verbatim |
