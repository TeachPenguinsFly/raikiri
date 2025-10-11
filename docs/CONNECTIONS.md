# One socket per venue is the ceiling everything else hits

Every adapter opens **one** market-data websocket and asks it for everything.
Venues cap what a single connection may carry, and `gatewayd` asks for four
topics per instrument — top of book, trades, depth and funding — so the ceiling
divides by four before it reaches an instrument count:

| venue       | subscribed | catalogue | share |
| ----------- | ---------: | --------: | ----: |
| bybit       |        815 |       815 |  100% |
| binance     |        342 |       571 |   60% |
| kucoin      |        256 |       673 |   38% |
| gate        |        256 |       979 |   26% |
| deribit-opt |        256 |      1812 |   14% |

Measured 2026-09-06. Everything past the cut is not subscribed at all, so its
row on the markets board is blank for as long as the process runs — which reads
as a dead venue rather than an unwatched instrument.

Two things were fixed first, because they were cheaper and they were lies
rather than limits: `gatewayd` collected the refusal reason and threw it away
unless *every* subscription failed, and every adapter sent its SUBSCRIBE frames
in a tight loop on connect. Venues drop a client that floods them, so binance
answered a full catalogue with **zero** books, trades and depth while
announcing 342 instruments. Paced, and with the quote channel sent first, the
same catalogue delivers: kucoin went from 30 books to 45 703, binance from 0 to
59 574.

Pacing does not raise the ceiling. That needs more connections.

`net::WsPool` is that, and gate was the first venue on it. Measured against the
live venue, one run of sixty seconds:

| gate                | before  | on the pool |
| ------------------- | ------: | ----------: |
| instruments subscribed |    256 |     **979** |
| subscriptions refused  |    723 |       **0** |
| top of book in 60 s    |      — |      50 684 |
| depth in 60 s          |      — |      19 539 |

The catalogue stopped being a quarter of itself. What the pool spends for that
is sockets — four for gate's 3916 channels at 1024 each — and a few seconds at
startup, because the asks are paced per connection rather than fired at once.

## The pool

A `WsPool` per adapter, holding N connections, each with a subscription set and
a capacity. `subscribe` finds a connection with room or opens another; every
connection's messages arrive on one handler, so nothing above the pool knows
there is more than one socket. This is the shape the Go service used, with
50 streams per connection as its default.

What it has to get right, and what the current single socket already gets right
by accident:

- **The reconnect is per connection.** Each one re-sends only its own
  subscriptions, so a socket that drops does not resubscribe the whole
  catalogue and trip the flood limit that pacing exists to avoid.
- **Pacing is per connection too.** The allowance venues state is per
  connection, so N sockets is N times the budget — but only if the sends are
  spaced on each of them, not globally.
- **Opening is lazy.** A connection nobody has subscribed anything to is an
  idle socket the venue may kick, and a reconnect storm on startup.
- **Books belong to a connection.** A depth book seeded on one socket cannot be
  fed by another, so the instrument's every topic must land on the same one.

## The REST side of the same problem, which is worse

`RateBudget` exists and is spent — **by the order path only**. The market-data
pools consult nothing, and that is what earned an IP ban twice on 2026-09-06:

    342 instruments x weight 20 (/fapi/v1/depth?limit=1000) = 6840 weight,
    fired as a burst at startup, against binance's 2400 per minute.

Nearly three times the whole minute's allowance, on every gateway start, from a
path no counter in the process could see. `gatewayd`'s request ledger now
reports what each endpoint was called and how often, which is how this was
found; the fix is to route the market-data pools through the same `RateBudget`
the order path uses, and to pace the seeding rather than fan it out at once.

### The local budget is looser than the venue's, and by a factor of two

`RateLimiter` is a token bucket that **starts full** and refills continuously,
so over any one window a caller may spend the capacity it began with PLUS a
window's worth of refill — twice what the venue allows. Against binance's 2400
a minute, poll traffic can reach roughly 4560 inside one calendar minute and
the local gate will not have refused a thing.

It has not bitten: measured on a live 46-second run the whole market-data path
spent 1388 weight, because the subscribe pacing delivers first deltas slowly
enough that seeds arrive at about six a second. But a full 571-instrument seed
round is 2855 weight and would fit inside that gap, so the ceiling is holding
by the ramp rather than by the budget.

The bucket is the right model for a venue quoting a rate (bybit's 50 a second);
it is the wrong one for a venue quoting a fixed window (binance's 2400 a
minute), and `venues.yaml` currently expresses both the same way. Fixing it
means either a window counter beside the bucket or halving what a
fixed-window venue declares — a decision about the model, not a patch.

## Order

1. Meter the market-data REST pools. The ban is the loudest failure and the
   smallest change: the budget and the config already exist.
2. Pace book seeding, which is the burst that ban came from.
3. The pool, per venue, starting with the two whose catalogues are cut worst
   (deribit at 14%, gate at 26%). Seven venues are on it as of 2026-09-08 —
   gate, okx, bybit, kucoin, bingx, bitget and mexc — and the spot section
   below is what the other six cost.

## What the pool cost, and has not paid back yet

`ConnectivityMsg` carries a `streamId` described in `schema/journal.xml` as
"which WS within the gateway". Every venue passes **0**, which was honest while
there was one socket. Under a pool the adapter's `on_open` fires once per
connection and still says 0, so the journal can no longer tell "socket three
came back" from "the venue came back" — and on a pool of eight that is eight
identical records where there used to be one meaningful one.

The pool knows the index; the adapter does not, because the shared handler is
deliberately not told which connection called it. Fixing it means widening
`WsPool::OnConnectionOpen` to carry the connection's index alongside its keys,
and each adapter putting that in `stream_id`. Small, and worth doing before
anybody tries to diagnose a flapping socket from the journal.

## The loop was the cost, not the data

Measured on okx, release build, 60-second windows after the subscribe ramp:

| | spinning | parked 1 ms |
| --- | ---: | ---: |
| one symbol (5 940 messages) | 35.5% of a core | 14.0% |
| full catalogue (556 681 messages) | 37.8% of a core | 15.6% |

Ninety-four times the messages cost **two and a half percentage points**. Almost
all of what a gateway spent was the loop going round, not the data going
through — which is also why subscribing fewer channels saved nothing measurable
(43.8% with everything, 43.0% with depth and trades off, on the same venue).

`gatewayd` now parks for 1 ms after 64 consecutive passes that drained no
command. Trading is unaffected while commands keep arriving; the cost is up to
1 ms on the first command after a lull, against a venue round trip measured in
tens of milliseconds.

Live, nine venues on their full catalogues: **4.2 cores before, 0.76 after**,
with coverage unchanged at 93-100% per venue (`tools/coverage_smoke.sh`).

## Also waiting: a browser test

Nothing in this repository opens the terminal. Every bug on 2026-09-06 that
showed as an empty screen was found by hand, and one of them — a `useMemo` over
a mutated map that never changed identity, so the table never recomputed — was
invisible to every check that stops at the socket, `tools/terminal_smoke.sh`
included. One Playwright test that opens Markets, picks a pair and waits for a
number in the Bid column would have caught it. It costs a devDependency in a
frontend deliberately kept small, which is why it is written down here rather
than added.

## Which venues actually stream SPOT

`caps.cpp` marks thirteen venues `.spot = true`, and that bit says what the
VENUE offers — never that the adapter can reach it. Grep cannot settle the
difference either: okx serves spot and swap on ONE socket keyed by `instId`, so
the absence of a `MarketType::Spot` branch proves nothing, and coinbase is
spot-only, so every line in it is a spot line.

So each of the thirteen was run, 90 seconds, two venues at a time:

    gatewayd --venue=<v> --market=spot --duration 90

Every number below is that process's own `down —` line. Measured 2026-09-08.

| venue | catalogue | subscribed | refused | bbo / 90 s | verdict |
| ----------- | --------: | ---------: | ------: | ---------: | ------- |
| gate        |      2166 |   **2166** |       0 |     48 136 | whole catalogue |
| okx         |      1406 |   **1406** |       0 |    118 303 | whole catalogue, after the fix below |
| bitget      |      1296 |   **1296** |       0 |    336 806 | whole catalogue |
| kucoin      |       958 |    **958** |       0 |    112 073 | whole catalogue |
| bingx       |       616 |    **616** |       0 |     21 170 | whole catalogue |
| bybit       |       533 |    **533** |       0 |    133 468 | whole catalogue |
| coinbase    |       522 |        342 |     180 |      1 981 | capped at one socket |
| xt          |       486 |        342 |     144 |     20 255 | capped at one socket |
| hyperliquid |       326 |        250 |      76 |     **50** | capped, and the quote barely moves |
| mexc        |       375 |        160 |     215 |    140 307 | capped, after the fix below |
| bitfinex    |       183 |     **10** |     173 |     24 373 | capped at 30 channels |
| bitmex      |         7 |          7 |       0 |     **99** | listed but not traded |
| htx         |       587 |      **0** |     587 |        n/a | no spot socket exists |

htx is the one row with no `down —` line behind it: the gateway exits at startup
with "every subscription was refused", so there is no 90-second window to count
and none is claimed.

Six venues carry their whole spot catalogue. **Those six are the set a
spot·perp carry board can be built on today**, and between them they cover 6975
pairs. All six are on `net::WsPool`. Of the seven below them, four are on a
single `net::WsClient` and cut by its ceiling, one (mexc) is pooled and cut by a
per-connection ceiling of thirty, one (bitmex) fits only because its catalogue
is seven pairs, and one (htx) has no spot socket at all.

### The three defects this found, and fixed

**okx asked a spot pair for its funding rate.** `subscribe` opened
`funding-rate` and `mark-price` for every instrument regardless of market.
Measured against `ws.okx.com`: `funding-rate` on instId `BTC-USDT` is refused
with code 60018, and `mark-price` on the same id answers `instType: MARGIN` — a
margin mark, which `FundingMemo` then published as a `FundingUpdate` carrying no
rate at all. That was the 69 522 "funding" events a spot run reported on a
market with no funding. The two channels also cost a third of the pool: at six
channels an instrument, 6 sockets x 1024 covers 1024 of the 1406 spot pairs, and
382 were refused. Spot now opens four — `bbo-tbt`, `trades`, `books`,
`tickers` — and the catalogue fits with room to spare: 1406 of 1406, none
refused, funding correctly zero.

**htx mapped 587 spot pairs and the table rejected all 587.**
`map_spot_symbol` left `ContractKind::Unknown` on the reasoning that a spot pair
has no contract. `validate()` refuses `ContractKind::Unknown`, so every row was
dropped by `InstrumentTable::add` — inside `intern`, which `continue`s past a
refused row without counting it. The gateway announced "0 instruments loaded"
and exited, and no test covered the path. Setting `ContractKind::Linear`, as
every other spot mapper here already does, loads all 587.

**mexc's spot socket takes 30 subscriptions, not 512.** The per-connection
number was measured on the FUTURES tree, where 4744 subscriptions on one socket
were all accepted. `wbs-api.mexc.com` is the other way round: 35 bookTicker asks
on one socket got 30 plain acks and then

    "Subscribed successful! []. Not Subscribed successfully! [...].
     Reason： Exceeded maximum subscription limit! "

— and that refusal carries **`code: 0`**, indistinguishable from an acceptance
to anything reading the field. So asking for 512 did not fail loudly; the venue
kept the first 30 keys of each socket and dropped the rest in silence. Since
`build_frames` sends the quote first, the 30 that survived were all quotes,
which is exactly what the first run showed: 78 651 books and **zero** trades and
**zero** depth. With the spot cap set to the venue's 30 the same run delivers
27 426 depth updates and 813 trades, and the 215 instruments that no longer fit
are REFUSED out loud instead of dropped by the venue in silence.

### What is still missing, per venue

**coinbase, xt, hyperliquid and bitfinex are on the pool now**, measured
2026-09-08. Every one of the four was refusing its own catalogue to a number
nobody had measured, and every one hid a different second defect behind that:

| venue | before | after | what the ceiling was hiding |
| --- | ---: | ---: | --- |
| coinbase | 342 of 522 | **522** | 1024 read off nothing, applied to the PROCESS |
| xt spot | 342 of 486 | **486** | the public socket closed every 30 s, unnoticed |
| xt perp | 256 of 657 | **657** | the same |
| hyperliquid | 250 of 326 | **326** | a 256-slot send queue ate 74% of the ask |
| bitfinex | 10 of 183 | **183** | `desired` was a 30-slot array used as a process cap |

**Three of the four answer an over-subscription in a way nothing above could
see.** XT never refuses at all — it RESETS the socket, so a ceiling set too high
appears only as reconnect churn and lost throughput. hyperliquid refuses on a
channel called `error` and keeps serving the thousand it took. bitfinex answers
`10305` on some servers and not others, because a connection's tier is the
server's choice and each reports a different `serverId`. Only a count of
DISTINCT INSTRUMENTS DELIVERING separates any of them from a quiet market.

Two findings worth carrying beyond their venues:

- **bitfinex channel ids belong to a SERVER, not to a socket.** Two connections
  landing on one server draw from one sequence, and connections on different
  servers were measured 329 apart. A registry shared across a pool would
  eventually serve one instrument's book under another's name, so the adapter
  runs one pool per socket and each keeps its own registry.
- **hyperliquid's `l2Book` is a heartbeat, not an event** — a full snapshot
  every ~5 s whether the book moved or not — and `trades` replays a backlog on
  subscribe. Neither channel's rate says anything about activity, which is how
  a sample of the catalogue's dormant end read as a working venue.

**mexc — the pool, but more of it.** 16 sockets at the venue's real 30 keys is
480 keys, which is 160 of 375 pairs. Covering the catalogue needs 38 sockets and
a measurement of what MEXC allows one IP. Neither has been made, so 16 stands.

**hyperliquid — and something else the cap does not explain.** 50 top-of-book
updates in 90 seconds across 250 pairs, against 1137 depth updates and 1921
trades on the same socket. `bbo` fires only when the top moves and hyperliquid's
spot pairs are thin, so this may be honest; a 23-fold gap between `bbo` and
`l2Book` on one connection is not obviously honest. Not chased down. **Do not
count hyperliquid spot as a quote source until it is.**

**bitmex — 7 pairs that are listed rather than traded, and a book that never
lands.** All 7 open `IFXXXP` pairs load and subscribe, and the venue answers:
99 quotes, 2 trades and 7 instrument rows in 90 s. The spread is why — XBT_USDT
quoted 74501.5 / 78430.5 live, five percent wide. Depth stays at **0** for a
different and real reason: BitMEX's aggregated `orderBookL2_25` publishes sizes
that are not multiples of the row's own `lotSize` (9 992 000 and 44 000 against
a lotSize of 10 000 on XBT_USDT), `Book::upsert` requires an exact conversion,
and one such level makes the whole `partial` return `BadLevel` and invalidate
the book. Deciding what a size off the lot grid means is a domain question, not
a spot question, and it was left alone.

**htx — a socket that does not exist.** This is the one venue needing real new
adapter surface. `HtxData::subscribe` refuses anything but `MarketType::Perp`
and says why: `api.hbdm.com` is the futures tree, and the only spot host this
adapter reaches is the private v2 stream. HTX's public spot feed is another
host (`api.huobi.pro/ws`), another topic grammar (`market.btcusdt.bbo`,
`market.btcusdt.trade.detail`, `market.btcusdt.depth.step0`) and its own
gzip framing. That is a whole dialect, not a channel name, and it was not
written. The catalogue fix above still matters: the gateway now fails with
"every subscription was refused — htx spot market data", which names the missing
piece, instead of "0 instruments loaded", which named nothing.

### The pool is the finding

Six of the seven pooled venues carry 100% of their spot catalogues; the four
cut by a single socket carry 5%, 66%, 70% and 77% of theirs. No dialect problem
separates the two groups — coinbase, xt, hyperliquid and bitfinex all subscribe
correctly to everything they are allowed to ask for, and every instrument below
their cut is simply never asked for at all. What separates them is one
connection against several, which is the same finding the top of this file
records for the perpetual side, arrived at again from the other direction.

The seventh pooled venue, mexc, is the reminder that a pool is only as good as
the per-connection number it is given: 512 was measured on the wrong tree, and
16 sockets times the right number is still short of the catalogue.
