# hyperliquid fixtures

Provenance matters here: a parser test is only evidence if the bytes are the
bytes the venue actually sends.

Every file below was **captured live** from production Hyperliquid on
2026-08-10. No credentials are involved — `/info` and the public websocket need
none. Nothing in this directory is hand-written or reshaped from docs.

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
