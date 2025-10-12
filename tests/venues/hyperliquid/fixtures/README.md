# hyperliquid fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Almost everything here was **captured live** from production Hyperliquid — the
market-data files on 2026-08-10, the account reads and the `/exchange` refusal
later — and needed no credentials to obtain: `/info` and the public websocket
are unauthenticated, and `/info` takes the wallet address in a plain unsigned
body because the address is public.

**Five files are the exception and say so where they appear**: the successful
`/exchange` replies under "REST — `POST /exchange`" are transcribed from the
venue's documentation and from a response recorded in another client, because a
successful order reply cannot be obtained without placing a real order with real
money. Nothing else in this directory is hand-written.

## REST — `https://api.hyperliquid.xyz/info`

There are no REST **GET** endpoints for market data on this venue. Every read
is a POST to the single `/info` path with a JSON body naming the request type,
which is why these files are named for a request rather than for a path.

| file | source |
|---|---|
| `rest_meta.json` | `POST /info` `{"type":"meta"}` (232 rows) trimmed to four |
| `rest_meta_and_asset_ctxs.json` | `POST /info` `{"type":"metaAndAssetCtxs"}` trimmed to the same four assets and their four contexts |
| `rest_null_body.json` | the body of `POST /info` `{"type":"l2Book","coin":"NOSUCHCOIN"}` — **HTTP 200** with a payload of the bare literal `null` |

The two trimmed files are assembled only in the sense that rows were
**selected**: each row is verbatim from a live response, re-emitted inside the
envelope the venue sent. `marginTables` is the one field emptied rather than
trimmed — the real response carries 60+ tables and nothing here reads them.

The four assets are the ones that make a wrong loader visible:

- `BTC` (`szDecimals` 5) and `ETH` (4) — the ordinary case, and the one where a
  price step of `10^-(6 - szDecimals)` is coarse (0.1 and 0.01);
- `MATIC` — `isDelisted: true`. It keeps its slot in the universe forever and
  still quotes a stale `prevDayPx`, so a loader that reads prices before
  checking the flag admits a symbol no book will ever update. Its context is
  also the one shipping `midPx`, `premium` and `impactPxs` as JSON **null**;
- `XRP` — `szDecimals` 0, the extreme of the derivation: sizes are whole coins
  and the price step is `1e-6`. A hardcoded step gets this row wrong.

`rest_null_body.json` is five bytes and earns its place: the venue answers a
query it cannot satisfy with **HTTP 200**, so nothing but the body
distinguishes a refusal from a successful read. A catalogue loader that trusted
the status would start up believing the venue lists nothing.

Other live `/info` failures, kept here as a record because none of them carries
a numeric code — the status is the only structured signal there is:

| request | answer |
|---|---|
| `{oops` | 400 `Failed to parse the request body as JSON` |
| GET instead of POST | 405 |
| POST with no `Content-Type` | 415 ``Expected request with `Content-Type: application/json` `` |
| `{}` or `{"type":"nosuchtype"}` | 422 `Failed to deserialize the JSON body into the target type` |
| `{"type":"meta","dex":"nosuchdex"}` | 500, body `null` |

## REST — account reads, `POST /info`

Captured live against production from one real trading account, and every row
is verbatim. Where a file is trimmed, rows were only ever **selected** — none
was edited. No credential was used: each of these is a plain unsigned POST
naming a public address, and none of the responses echoes it back.

| file | source |
|---|---|
| `rest_clearinghouse_state.json` | `{"type":"clearinghouseState","user":"0x…"}` — perpetual positions and margin |
| `rest_spot_clearinghouse_state.json` | `{"type":"spotClearinghouseState","user":"0x…"}` — spot BALANCES |
| `rest_open_orders.json` | `{"type":"openOrders","user":"0x…"}` |
| `rest_frontend_open_orders.json` | `{"type":"frontendOpenOrders","user":"0x…"}` — the same four orders |
| `rest_order_status.json` | `{"type":"orderStatus","user":"0x…","oid":527278286739}` |
| `rest_order_status_unknown.json` | the same request with an oid the venue has never minted |
| `rest_user_fills.json` | `{"type":"userFills","user":"0x…"}`, trimmed to five rows |
| `rest_spot_meta.json` | `{"type":"spotMeta"}` (326 pairs, 500 tokens) trimmed to seven pairs and the eight tokens behind them |

What each one is here to prove:

- `rest_clearinghouse_state.json` — `szi` is **signed** and no side field sits
  beside it, so a reader taking the magnitude reports every short as a long.
  Three of the four coins are in `rest_meta.json` and **FARTCOIN is not**: a
  live perp this trimmed catalogue does not carry must map to nothing rather
  than to a neighbour's steps. The `time` at the end is the only timestamp in
  the response — no row carries its own, and it trails `assetPositions`, which
  is why the parser reads it after the array rather than before.
- `rest_spot_clearinghouse_state.json` — the evidence for a refusal rather than
  for a mapper. There is no position and no entry price anywhere in it: one
  `HYPE` balance backs `@107` and every other pair quoting HYPE, so
  `position_reports(Spot)` says it cannot answer instead of reporting flat.
- `rest_open_orders.json` and `rest_frontend_open_orders.json` — **the same four
  orders**: identical `oid`, `timestamp`, `limitPx`, `sz` and `origSz` in both.
  The pair is the argument for reading the frontend form. The plain form omits `tif`, `orderType`, `reduceOnly` and
  `isTrigger` entirely, so a dormant conditional arrives there looking exactly
  like a limit resting at its trigger price. `tif: "Alo"` in the frontend form
  is the only place post-only appears in this API. Both carry a perpetual
  (`ETH`) beside spot pairs (`@334`, `@107`) in **one array**, which is why the
  market filter is the instrument's own and not a second request. `cloid` is
  **absent** on the orders placed without one in the plain form and **JSON
  null** in the frontend form — two spellings of the same nothing.
- `rest_order_status.json` / `rest_order_status_unknown.json` — an order id the
  venue has forgotten and one it never minted answer identically, with
  `{"status":"unknownOid"}`, and neither is an error. The found form nests the
  order two levels deep and puts a second `status` **beside** the inner object,
  shadowing the outer one.
- `rest_user_fills.json` — five rows chosen for what they break. The `ETH` fill
  has `crossed: false` and a **negative** `fee`: the venue paid us a maker
  rebate, and a reader treating fee as an unsigned magnitude turns it into a
  cost. `PURR/USDC` is a `Spot Dust Conversion` whose `sz` of `0.99911` does
  **not** divide by PURR's whole-coin lot — the venue's own sweep ignores the
  lot size, and the row must be refused rather than rounded. The two `xyz:`
  rows are a builder-deployed perp DEX: they are real fills on a real account
  and belong to a catalogue this adapter never loads, so they resolve to
  nothing.
- `rest_spot_meta.json` — the pair rows name their tokens by **index** and carry
  no precision of their own; `szDecimals` lives on the base token, so a mapper
  handed only the pair would have to guess the step. `PURR/USDC` is the one
  canonical pair spelled by name; `@107` and `@334` are how every other pair is
  spelled everywhere the venue mentions them. Their `index` is **not** their
  position in the array, and the asset id an order names is `10000 + index` —
  a different space, in which asset 107 is an unrelated perpetual.

  The four `HYPE` rows are the reason this file grew. `tokens` is
  `[base, quote]` and **the quote is not always USDC**: 17 of the 326 live pairs
  settle in `USDH`, `USDT0` or `USDE`, and twelve of those bases also list
  against USDC. `@107`, `@207`, `@232` and `@255` are one base token against
  four quote tokens — four separate books at four separate prices, 85.8975 and
  85.912 on the first two when this was captured — so a canonical name built
  from the base alone gives one account four listings of "HYPE/USDC" and a
  cross-venue join four candidates for one leg. `@252` is `AXL/USDH`, which
  lists against **nothing else**: refusing the rows that do not quote USDC would
  hide a market rather than de-duplicate one.

## REST — `POST /exchange`

`exchange_err_bad_signature.json` was **captured live** on 2026-08-26 by posting
a syntactically valid request whose `r`/`s` are not a signature. It places
nothing and needs no credentials, and it establishes the one fact the whole
error path rests on: a top-level refusal arrives with **HTTP 200**.

```
$ curl -X POST https://api.hyperliquid.xyz/exchange -H 'Content-Type: application/json'     -d '{"action":{"type":"cancel","cancels":[{"a":0,"o":1}]},"nonce":1787746850812,
         "signature":{"r":"0x1111…","s":"0x2222…","v":27},"vaultAddress":null}'
{"status":"err","response":"Unable to recover signer."}     [HTTP 200]
```

The other malformed-request answers, captured the same way, do **not** use the
envelope at all — they are bare prose with a real status code:

| request | answer |
|---|---|
| `{oops` | 400 `Failed to parse the request body as JSON` |
| `{"action":{"type":"nosuchaction"},…}` | 422 `Failed to deserialize the JSON body into the target type` |

The remaining five files are **not live captures**, and are marked as such
because a successful `/exchange` reply cannot be obtained without placing a real
order with real money:

| file | source |
|---|---|
| `exchange_order_resting.json` | the venue's own API documentation, verbatim |
| `exchange_order_filled.json` | the venue's own API documentation, verbatim |
| `exchange_order_error.json` | a real response body recorded in ccxt's Hyperliquid adapter |
| `exchange_cancel_success.json` | the venue's own API documentation, verbatim |
| `exchange_cancel_missing.json` | the venue's own API documentation, verbatim |

`exchange_order_error.json` earns its place twice over. It is `status: "ok"` —
**a refused order is a successful request**, and only a malformed or
unauthorised request reaches `status: "err"`, so a reader that trusts the outer
status reports every rejection as an acceptance. And its prose carries the
suffix `asset=84`, which the documented error table does not show: the venue
appends context to the refusals a strategy meets most often, so the
classification matches by prefix rather than by equality.

`exchange_cancel_success.json` is the one whose `statuses` element is a bare
**string** rather than an object, so a reader assuming every element is an
object drops the only answer a cancel ever gives.

## Signing vectors — no fixture file

The signing tests carry their vectors inline rather than as fixtures, because
they are not venue responses: they are the expected outputs published in
`hyperliquid-dex/hyperliquid-python-sdk`, `tests/signing_test.py` and
`hyperliquid/utils/signing.py` (read 2026-08-26). Eight `{r, s, v}` triples, one
action hash, and the canonical keccak-256 vectors. RFC 6979 makes signing
deterministic, so those are equalities rather than "verifies against the public
key" — the strongest evidence obtainable without a funded account.

## Websocket — `wss://api.hyperliquid.xyz/ws`

One session per subscription type, each read for 12–20 s.

| file | source |
|---|---|
| `ws_subscribe_ack.json` | the `l2Book` acknowledgement, on channel `subscriptionResponse` |
| `ws_bbo.json` | first `bbo` push on `BTC` |
| `ws_trades.json` | a `trades` push carrying one print |
| `ws_trades_batch.json` | a `trades` push carrying two, one taker against two makers |
| `ws_l2book_snapshot.json`, `ws_l2book_next.json` | **consecutive** `l2Book` frames off one socket |
| `ws_active_asset_ctx.json` | first `activeAssetCtx` push on `BTC` |
| `ws_error.json` | from a session that subscribed to `{"type":"nosuchchannel"}` |
| `ws_pong.json` | the answer to `{"method":"ping"}` |
| `ws_all_mids.json` | an `allMids` push, trimmed to four mids |

The two `l2Book` files are the evidence for the claim the adapter's book
handling rests on. They are **consecutive frames off one socket**, both carry a
full 20 levels a side, both carry a fresh `time`, and neither carries a
sequence id, a previous-sequence id or an `action` field — there is no field
anywhere in the payload that could chain one frame to the next. Every `l2Book`
message is a full snapshot, so there is no continuity to validate and no
resync path to get wrong. This is the one place this venue is *simpler* than
okx and binance rather than stranger.

`ws_error.json` is worth reading closely: `data` is a plain **string**, not an
object, and there is no code field. The venue also **closed the connection**
immediately after sending it, so a single bad subscription costs every other
subscription on that socket.

`ws_all_mids.json` is a channel this adapter does **not** subscribe to. It is
kept so a test can pin that a real channel the dispatcher does not claim
resolves to `Unknown` and is dropped rather than taken as a malformed frame.
Its keys are also a warning: alongside `BTC` and `ETH` it carries spot pairs as
`@1` and other markets as `#10490`, so it is not a perp-only feed.

## Not captured

An unknown **coin** on a valid subscription type. There is nothing to capture:
the venue answers it with total silence — no acknowledgement, no error, no
data — verified twice, once on a fresh socket and once on a socket already
receiving `BTC` trades. A typo'd symbol is therefore indistinguishable from a
quiet market, which is why the adapter refuses an unknown instrument before it
reaches the wire.

There is no rate-limit fixture. Reproducing a 429 means hammering a production
endpoint, and the classification it feeds is decided by the HTTP status alone.
