# bitget fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file below was **captured live** from production Bitget on 2026-08-11.
No credentials are involved — all of these are public endpoints and the public
websocket. Nothing in this directory is hand-written or reshaped from docs, and
nothing was round-tripped through a JSON library: files were spliced out of the
raw capture by text, so `0.0001` is still `0.0001` and not `1e-04`.

## Websocket — `wss://ws.bitget.com/v2/ws/public`

One session subscribing `ticker`, `trade` and `books5` on `BTCUSDT`, read for
14 s; `ws_books15.json` was recaptured 2026-08-17 on its own session.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `ticker` acknowledgement of that subscribe |
| `ws_ticker.json` | one `ticker` push — bid/ask, mark, index and funding on one frame |
| `ws_trade.json` | a `trade` push carrying four prints |
| `ws_books15.json` | a `books15` push: fifteen levels a side, `pseq` 0 |

`books` (the incremental 500-level channel) came from a second session on
`BTCUSDT`:

| file | source |
|---|---|
| `ws_books_update.json` | a delta containing a `"0"` level, which is a delete |
| `ws_books_update_one_sided.json` | a delta with `"bids":[]` — a real live frame, not a malformed one |

The two error frames came from a third session:

| file | source |
|---|---|
| `ws_error_unknown_instrument.json` | subscribing `instId=NOSUCHCOINUSDT` → `code:30001` |
| `ws_error_unrecognized_request.json` | sending `ping\n` instead of `ping` → `code:30002` |

Two things those error frames pin. The websocket `code` is an **unquoted JSON
number** while the REST envelope's is a quoted string — the same adapter needs
both reads. And the keepalive really is exactly four bytes: `ping` is answered
with the bare text `pong` (verified separately), while one trailing newline
comes back as `Unrecognized request:ping\n`. That second file is also the only
payload here containing a JSON escape, which is why the parser's
raw-token-no-unescape rule is tested against it.

`pong` itself is not a fixture: it is not JSON and the test asserts the parser
refuses the literal.

## REST — `https://api.bitget.com`

| file | source |
|---|---|
| `rest_contracts.json` | `GET /api/v2/mix/market/contracts?productType=USDT-FUTURES` (745 rows), trimmed to four |
| `rest_ticker.json` | `GET /api/v2/mix/market/ticker?symbol=BTCUSDT&productType=USDT-FUTURES` |
| `rest_orderbook.json` | `GET /api/v2/mix/market/orderbook?symbol=BTCUSDT&productType=USDT-FUTURES&limit=5` |
| `rest_current_fund_rate.json` | `GET /api/v2/mix/market/current-fund-rate?symbol=BTCUSDT&productType=USDT-FUTURES` |
| `rest_server_time.json` | `GET /api/v2/public/time` |
| `rest_error_unknown_symbol.json` | the ticker call with `symbol=NOSUCHUSDT` — a real `40034` body |
| `rest_error_missing_product_type.json` | the ticker call with `productType` omitted — a real `400172` body |

`rest_ticker.json` and `rest_orderbook.json` exist to pin **divergences**, not
similarities. The REST ticker names the instrument `symbol` where the websocket
names it `instId`, ships no `arg`, and omits `nextFundingTime` entirely, so the
websocket parser must refuse it rather than share a code path. The orderbook is
the one endpoint whose `data` is an OBJECT rather than a one-element array.

`rest_contracts.json` is the only assembled file, and only in the sense that
rows were **selected**: each of the four is a verbatim row from the live
response, re-emitted by text inside the envelope the venue sent. It keeps

- `BTCUSDT` and `ETHUSDT` — normal perpetuals, both loadable, with different
  price and size precision (0.1/0.0001 against 0.01/0.01);
- `SHIBUSDT` — `volumePlace` 0 but `sizeMultiplier` 10000. Reading the display
  precision as the quantity step would accept sizes the venue cannot produce;
  reading `pricePlace` 9 gives a tick of 0.000000001, the finest the domain's
  e9 fixed point can hold;
- `PEPEUSDT` — `pricePlace` 10. **The mapper must refuse it.** A tenth decimal
  floors to a zero tick, and a zero tick divides every later price conversion.
  A live catalogue load confirms the count: 745 rows in, 744 instruments out.

A loader that accepts all four is wrong in a way that stays invisible until an
order prices off a zero step.

## The `priceEndStep` claim, measured

Bitget's documented tick is `priceEndStep * 10^-pricePlace`, and the adapter
implements the product. **It cannot be distinguished from `10^-pricePlace`
against live data today**: on 2026-08-11 `priceEndStep` was `"1"` on all 745
`USDT-FUTURES` rows, all 49 `USDC-FUTURES` rows and all 11 `COIN-FUTURES` rows.
The only fixture that exercises the second factor is therefore a constructed
row inside the test (`pricePlace` 2, `priceEndStep` 5 → 0.05), and it is
labelled as constructed there. That is precisely why the factor is easy to drop
and impossible to notice until a row changes it.

## Built from the API reference

No credentials exist here, so the signed bodies below are assembled from
Bitget's own reference. Field NAMES and TYPES are the doc's; the symbols were
changed to ones `rest_contracts.json` actually carries, so the mapper divides
by real steps.

| file | reference shape |
|---|---|
| `rest_mix_account_bill.json` | `GET /api/v2/mix/account/bill` — the doc's two rows, re-symbolled, plus two written here |

Four rows, each isolating one mapper decision:

- a `contract_settle_fee` on `BTCUSDT` with `amount` **negative** — the doc's
  own row, verbatim but for the symbol and the bill id;
- the same businessType on `ETHUSDT` with `amount` **positive**. The pair is the
  whole point: `amount` is the signed balance move, and a reader taking its
  magnitude reports every payment as income;
- an `open_long` row carrying `amount: "0"` and `fee: "-0.222012"` — also the
  doc's. It must be **refused**. Its money is realised PnL and a commission the
  fill already booked, and the businessType filter that would normally keep it
  off the page is a query parameter, not a guarantee;
- a `contract_settle_fee` on `SOLUSDT`, which this catalogue does not carry, so
  the mapper is seen dropping it rather than guessing.

The `fee` column is read by nothing. A commission is compared, not booked, and
the comparison keys on the venue's id for the TRADE — which a bill row does not
carry at all.

## Not captured

No `books5` fixture. The channel exists and `kind_from_channel` claims it, but
it ships the identical element to `books15` and `books`, both of which are
captured here.

No `books50` or `books100`: neither exists. Both drew
`{"event":"error","code":30016,"msg":"Param error"}` on a fresh socket, which
is why 15 is the deepest snapshot tier this adapter can subscribe at. The
incremental `books` channel does open with 500 levels a side, but reaching them
means maintaining a local book across its `seq`/`pseq` chain.
