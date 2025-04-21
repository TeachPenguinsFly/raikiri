# Deribit fixtures

Captured 2026-08-11 against **production** `www.deribit.com` (`"testnet":false`
is visible in every REST body), unauthenticated. REST with `curl`, websocket
with `websocat -B 400000` against `wss://www.deribit.com/ws/api/v2`.

Every file is a verbatim frame or body reformatted to one line. Nothing is
hand-written or synthesised. The only edits are noted per file below, and both
are deletions — no number token, key or ordering was ever retyped, because the
exact spelling of a number (`10` vs `10.0` vs `1.0e-5` vs `6.4e4`) is the thing
several of these fixtures exist to pin.

## REST — `GET /api/v2/public/...`

| file | provenance |
| --- | --- |
| `rest_get_time.json` | `get_time`. Verbatim. `result` is a bare number, not an object. |
| `rest_get_instruments.json` | **Trimmed.** Six whole rows lifted unmodified out of four live bodies (`get_instruments` for `currency=BTC&kind=future`, `&kind=option`, `&kind=future&expired=true`, and `currency=any&kind=future`) and concatenated under one envelope. Rows are byte-identical to the wire apart from removed whitespace; the surrounding envelope is the real one from the `kind=future` call. |
| `rest_error_invalid_kind.json` | Verbatim response to `get_instruments?currency=BTC&kind=perpetual`. Deribit has **no** `perpetual` kind — this is the error that proves it. |

The six catalogue rows, and why each is there:

| row | what it proves |
| --- | --- |
| `BTC-PERPETUAL` | the perpetual arrives under `kind: "future"` with `settlement_period: "perpetual"`; `instrument_type: "reversed"`; `contract_size: 10.0` USD |
| `BTC-25SEP26` | a dated future off the same call — must not land in the Perp market |
| `BTC-11AUG26-64000-C` | an option: `strike: 6.4e4` (exponent form), `option_type`, real `expiration_timestamp`, `min_trade_amount: 0.1` against `contract_size: 1.0` |
| `BTC-11AUG26-64000-P` | the put side of the same series |
| `BTC-10AUG26` | **the row the mapper must refuse**: `is_active: false`, `state: "delivered"` |
| `ADA_USDC-PERPETUAL` | **the second row the mapper must refuse**: `instrument_type: "linear"`, where `contract_size` counts base units instead of USD. Also carries `tick_size: 1.0e-5`. |

## WebSocket — `public/subscribe`

| file | channel / provenance |
| --- | --- |
| `ws_book_bbo.json` | `book.BTC-PERPETUAL.none.1.100ms`. Verbatim. |
| `ws_book_bbo_option.json` | `book.BTC-11AUG26-64000-C.none.1.100ms`. Verbatim. Size `23.1` is not a multiple of `contract_size` 1.0. |
| `ws_book_snapshot.json` | `book.BTC-PERPETUAL.100ms`, `type: "snapshot"`. **Trimmed**: the live frame was 43,942 bytes / 897 bids / 1,080 asks; the first 3 levels a side are kept unmodified and the rest deleted. `change_id` is the real one. |
| `ws_book_change.json` | `book.BTC-PERPETUAL.100ms`, `type: "change"`. **Trimmed** the same way (5 bids, all 4 asks kept). Its `prev_change_id` really does chain to `ws_book_snapshot.json`'s `change_id` — asserted in the test. Carries `new`/`change`/`delete` actions and an exponent-form price `1.641e5`. |
| `ws_trades.json` | `trades.BTC-PERPETUAL.100ms`. Verbatim. |
| `ws_ticker_perpetual.json` | `ticker.BTC-PERPETUAL.100ms`. Verbatim. Has `current_funding` + `funding_8h`; `mark_price: 64111.95` is off the 0.5 tick grid; `interest_value` carries 18 fractional digits. |
| `ws_ticker_future.json` | `ticker.BTC-25SEP26.100ms`. Verbatim. **No funding fields at all** — absence, not zero. |
| `ws_ticker_option.json` | `ticker.BTC-11AUG26-64000-C.100ms`. Verbatim. No funding; carries `greeks` and `mark_iv`, which the parser deliberately ignores. |
| `ws_subscribe_ack.json` | Verbatim reply to a 5-channel subscribe. `result` lists what was **accepted**. |
| `ws_subscribe_ack_empty.json` | Verbatim reply to `book.BTC-DOESNOTEXIST.none.1.100ms`. `result: []` with **no error**: an unknown instrument is refused silently, which is why the adapter counts accepted-vs-requested. |
| `ws_error_raw_unauthorized.json` | Verbatim. Asking for `ticker.BTC-PERPETUAL.raw` on an unauthenticated socket returns 13778 — and it killed the other three channels in the same batch, which is the evidence that `public/subscribe` is all-or-nothing. |
| `ws_heartbeat_test_request.json` | Verbatim, after `public/set_heartbeat` with `interval: 10`. Must be answered with `public/test` or the socket is closed. |

## Channels probed and not implemented

- `deribit_price_index.btc_usd` — ACKed and delivers (`{"price":...,"index_name":...}`); no domain type for an index level.
- `trades.any.any.100ms` — silently absent from the ack's `result`, i.e. refused.
- `ticker.<instrument>.raw` — refused with 13778 for an unauthenticated socket.
