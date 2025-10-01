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

`net::WsPool` is that, and gate is the first venue on it. Measured against the
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
   (deribit at 14%, gate at 26%).

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
