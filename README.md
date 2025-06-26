# raikiri 雷切

[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-16%20live-green)](#venues)
[![Tests](https://img.shields.io/badge/tests-1330-green)](#build)

A low-latency, multi-process crypto trading framework in C++23. Sixteen exchange
adapters run as separate processes and talk over lock-free shared memory — there is no
message broker on the hot path and no database write in front of an order. Durability
is an append-only SBE journal per process, tailed into PostgreSQL by a cold-plane
reader that the trading path never waits on.

Everything below was measured against the real venues. Where a capability is declared
but silent on the wire, this document says so.

```
===== demo: sim venue, 10s, single unpinned core (macOS dev laptop) =====
metric (ns)                count         p50         p90         p99
tick-to-callback           19631        1007        2015        7039
submit-call                  156         295        3391       14335
order-round-trip             156        8703       12543       27647
fills=139  exec-ring-overruns=0  fsm-refusals=0
```

`tick-to-callback` is a BBO written by a gateway process reaching a strategy callback
in the kernel process, across the seqlock table. `submit-call` is the risk gate, the
order store, the journal append and the command-ring push. `order-round-trip` is a
submit reaching the matching engine and its verdict coming back — two process
boundaries each way.

## Venues

Sixteen adapters, all verified against production endpoints with no credentials.

| Venue | Markets | Market data | Order entry | Reconciliation pulls |
| --- | --- | --- | --- | --- |
| bybit | spot, perp | full | WS | positions, fills |
| okx | spot, perp | full | WS | positions, fills |
| gate | spot, perp | full | WS | positions, fills |
| kucoin | spot, perp | full | WS | positions, fills |
| bitget | spot, perp | full | WS | positions, fills |
| bitfinex | spot, perp | full | WS | positions, fills |
| hyperliquid | spot, perp | full | WS | positions, fills |
| deribit | perp, future, **option** | full, with venue greeks | WS | positions, fills |
| kraken | perp | full | WS | positions, fills |
| xt | spot, perp | full, funding polled | REST | positions, fills |
| bingx | spot, perp | full, funding polled | REST | positions |
| mexc | spot, perp | full, spot is protobuf | REST | positions (perp only) |
| binance | perp | book and prints, funding polled <sup>1</sup> | WS + FIX | positions, fills |
| bitmex | perp, spot <sup>2</sup> | full | REST | positions, fills |
| htx | perp, spot <sup>3</sup> | full | REST | positions |
| coinbase | spot | full | REST | — <sup>4</sup> |
| sim | perp | generated | in-process | positions |

<sup>1</sup> `@aggTrade` and `@markPrice` are acknowledged on this venue and then send
nothing, while `bookTicker` and `trade` flood. Prints come from `@trade`; funding is
pulled from `/fapi/v1/premiumIndex`, one call for every symbol.
<sup>2</sup> Spot subscribes and stays silent — seven live pairs with real volume and
no frame in 25 s. An adapter defect, not a thin market.
<sup>3</sup> The spot catalogue interns none of its 600 tradable pairs, so a spot
gateway starts empty and exits.
<sup>4</sup> Coinbase Exchange lists no perpetuals and this adapter implements no
account pulls, so nothing can be reconciled against.

Reading the table: **full** means top-of-book, depth and prints all arrive.
A pull that is absent is stated rather than answered with an empty list — a venue that
cannot report and a venue reporting flat are different facts, and collapsing them is
how a system concludes it holds nothing while holding something.

**Not reachable yet:** account balances, on any venue. Nothing pulls them, so there is
no margin model and no free-collateral check. Funding is the published *rate*, never
the charge a venue actually took — no funding has moved the PnL ledger.

## How it works

Each gateway is one process serving one venue and one market — `gatewayd
--venue=deribit --market=option` — and owning a shared-memory segment: seqlock tables
for quotes, depth, funding, greeks and venue positions; broadcast rings for market data
and execution events; lossless SPSC rings for order commands. A single-threaded kernel
(`strategyd`) busy-polls every segment it is attached to, holds one book across all of
them, and fires orders back through the command rings. `persisterd` tails the journals
into Postgres.

The channel class is a design decision per channel, not a default. State — a quote, a
book, a position — is an overwrite-newest table, because a stale tick is worthless and a
reader that falls behind should skip rather than queue. Execution is a lossless ring,
because a fill that was dropped is money the book never learned about, and an overrun is
reported rather than absorbed.

- Money is fixed-point `int64` throughout. `double` is banned from decision code.
- A wire value that does not divide exactly by an instrument's tick or lot size is a
  schema event, not something to round over.
- Hot path: `std::expected`, preallocated, zero allocation in steady state. Cold path:
  exceptions and coroutines for connection orchestration.
- Production target is Linux x86-64; macOS is a supported dev and demo platform, with
  no core pinning or hugepages.

## Quick start

```sh
make deps          # conan install (PRESET=dev-asan by default)
make test          # build and run the suite under ASan/UBSan
make demo          # the whole pipeline against the built-in matching engine
```

`make demo` needs no API keys: a price-time-priority matching engine implements the
same gateway contract as a real venue, so gatewayd, strategyd and the journal all run
end to end.

Against a real exchange, still without credentials — market data needs none:

```sh
gatewayd --venue=deribit --market=option --symbol=BTC-28AUG26-40000-C --duration 30
gatewayd --venue=bybit   --market=spot   --symbol=BTCUSDT
./tools/venue_smoke.sh   # every venue at once, 16/16 expected
```

Order entry reads its credentials from a *reference*, never a value —
`--api-key=env:BYBIT_API_KEY`, `file:/run/secrets/key`, or `exec:op read op://...`, so
no key reaches a config file or a journal.

## Layout

| Path | What lives there |
| --- | --- |
| `apps/` | `gatewayd`, `strategyd`, `persisterd`, `replay`, `launcher` |
| `libs/domain` | instruments, orders, the order FSM, fixed-point money |
| `libs/venues` | the sixteen adapters, one directory each |
| `libs/shm` | segments, seqlock tables, broadcast and SPSC rings |
| `libs/journal` | the append-only mmap journal and its reader |
| `libs/portfolio` | positions, PnL, targets, reconciliation |
| `libs/oms` | order store, risk gate, id generation |
| `libs/simcore` | the matching engine behind the sim venue |
| `schema/` | `journal.xml` (SBE) and `admin-api.yaml` (the terminal's contract) |
| `frontend/` | maverick v3 — planned, see its README |
| `tests/`, `fuzz/`, `bench/` | 1330 tests, fuzz targets, microbenchmarks |

## Performance

Cross-process SPSC ping-pong over the shm transport, M-series macOS, unpinned. Linux
pinned-core numbers will be published from the production target rather than estimated
here.

| | round trip | one way |
| --- | --- | --- |
| p50 | 125 ns | 62 ns |
| p99 | 167 ns | 83 ns |
| p999 | 292 ns | 146 ns |

Primitive costs, Release, single process: seqlock write ≈ 1.8 ns, seqlock read ≈ 0.7 ns,
SPSC push+pop ≈ 1.8 ns, broadcast publish ≈ 2.3 ns.

## Build

Requires CMake ≥ 3.28, Ninja, Conan 2, and a C++23 compiler — GCC 14, Clang 18 or
AppleClang 17.

```sh
make deps                  # dependencies via Conan
make test                  # ASan + UBSan
make test PRESET=dev-tsan  # ThreadSanitizer
make bench                 # Release benchmarks
make fuzz FUZZ_SECONDS=60  # libFuzzer targets (Homebrew clang on macOS)
```

## Status

Working: sixteen venues streaming live market data; order entry on every one of them;
positions, PnL and an append-only ledger; reconciliation against what the venue says it
holds; Postgres persistence with idempotent replay; options and dated futures on
Deribit.

Not built yet: account balances and margin, funding credited from the venue's actual
charge, conditional and bracket orders, execution algorithms, a backtest harness over
recorded data, and the admin service and terminal the API contract describes.

## License

LGPL-3.0. The full text is in `LICENSE`; `COPYING` holds the GPL-3.0 text that
LGPL-3.0 incorporates by reference.

Chosen deliberately over a permissive licence. A strategy or an application that links
raikiri stays closed if its author wants it closed, while a fork of the framework
itself has to publish its changes. LGPL carries no network clause, so running raikiri
as a hosted service triggers no obligation either way.
