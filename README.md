<div align="center">

# raikiri

**A low-latency, multi-process crypto trading framework in C++23**

Sixteen exchange adapters, each its own process, talking over lock-free shared memory.<br>
No message broker on the hot path. No database write in front of an order.

[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-16%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-1413-2da44e?style=flat-square)](#build)
[![Sanitizers](https://img.shields.io/badge/ASan%20·%20UBSan%20·%20TSan-green?style=flat-square)](#build)

[Venues](#venues) · [Architecture](#architecture) · [Quick start](#quick-start) ·
[Performance](#performance) · [API contract](schema/admin-api.yaml) · [Status](#status)

</div>

---

Every table below was measured against production endpoints. Where a capability is
declared but silent on the wire, this document says so — a framework that reports what
it wishes were true is worse than one that reports nothing.

```text
===== demo: sim venue, 10s, single unpinned core (macOS dev laptop) =====
metric (ns)                count         p50         p90         p99
tick-to-callback           19631        1007        2015        7039
submit-call                  156         295        3391       14335
order-round-trip             156        8703       12543       27647
fills=139  exec-ring-overruns=0  fsm-refusals=0
```

<sup>**tick-to-callback** — a quote written by a gateway process reaching a strategy
callback in the kernel process, across the seqlock table. **submit-call** — risk gate,
order store, journal append and command-ring push. **order-round-trip** — a submit
reaching the matching engine and its verdict coming back, two process boundaries each
way.</sup>

---

## Architecture

```mermaid
flowchart LR
    subgraph V["exchanges"]
        direction TB
        E1["16 venues<br/>WS · REST · FIX"]
    end

    subgraph G["gatewayd — one process per (venue, market)"]
        direction TB
        GA["adapter<br/>parse · map · normalise"]
    end

    subgraph S["shared memory · one segment per gateway"]
        direction TB
        T["seqlock tables<br/><i>quotes · depth · funding<br/>greeks · venue positions</i>"]
        R["broadcast rings<br/><i>market data · execution</i>"]
        C["SPSC rings<br/><i>order commands</i>"]
    end

    subgraph K["strategyd — single-threaded kernel"]
        direction TB
        ST["one book across every<br/>attached market"]
    end

    J["append-only SBE journals<br/><i>mmap, one per process</i>"]
    P[("PostgreSQL<br/><i>cold plane</i>")]
    A["admind<br/><i>HTTP + WebSocket</i>"]

    E1 --> GA
    GA --> T & R
    C --> GA
    T & R --> ST
    ST --> C
    GA -.-> J
    ST -.-> J
    J --> P
    T --> A
    J --> A
    P --> A
    A --> W(["terminal"])
```

Each gateway is one process serving **one venue and one market** —
`gatewayd --venue=deribit --market=option` — and owning a shared-memory segment. A
single-threaded kernel busy-polls every segment it is attached to, holds one book
across all of them, and fires orders back through the command rings. `persisterd`
tails the journals into Postgres; the trading path never waits on either.

**The channel class is a decision per channel, not a default.**

| Channel | Kind | Why |
| :-- | :-- | :-- |
| quotes, depth, funding, greeks, venue positions | seqlock table, overwrite-newest | a stale tick is worthless — a reader that falls behind should skip, never queue |
| market data, execution events | broadcast ring, gap-detecting | a fill that was dropped is money the book never learned about; an overrun is reported, not absorbed |
| order commands | SPSC ring, lossless | the escape hatch must work precisely when something is wrong |

<details>
<summary><b>Invariants the code is built on</b></summary>

<br>

- Money is fixed-point `int64` throughout. `double` is banned from decision code — a
  double round-trip is exactly how a price becomes off by a tick.
- A wire value that does not divide exactly by an instrument's tick or lot size is a
  **schema event**, never something to round over.
- Absence of evidence is not evidence of flatness. A venue that cannot report, a pull
  that failed, and a venue reporting zero are three different facts; collapsing them is
  how a system concludes it holds nothing while holding something.
- Hot path: `std::expected`, preallocated, zero allocation in steady state.
  Cold path: exceptions, coroutines for connection orchestration.
- Production target is Linux x86-64. macOS is a supported dev and demo platform, with
  no core pinning or hugepages.

</details>

---

## Venues

<div align="center">

**16 adapters · 4 market types · all verified against production endpoints, no credentials**

</div>

| Venue | Markets | Market data | Order entry | Reconciliation | Balances |
| :-- | :-- | :-- | :-- | :-- | :--: |
| **bybit** | spot · perp | full | WS | positions · fills | ✅ |
| **okx** | spot · perp | full | WS | positions · fills | — |
| **gate** | spot · perp | full | WS | positions · fills | ✅ |
| **kucoin** | spot · perp | full | WS | positions · fills | ✅ |
| **bitget** | spot · perp | full | WS | positions · fills | — |
| **bitfinex** | spot · perp | full | WS | positions · fills | — |
| **hyperliquid** | spot · perp | full | WS | positions · fills | — |
| **deribit** | perp · future · **option** | full + venue greeks | WS | positions · fills | — |
| **kraken** | perp | full | WS | positions · fills | — |
| **xt** | spot · perp | full, funding polled | REST | positions · fills | ✅ |
| **bingx** | spot · perp | full, funding polled | REST | positions | — |
| **mexc** | spot · perp | full, spot is protobuf | REST | positions <sub>perp only</sub> | — |
| **binance** | perp | book + prints <sup>[1]</sup> | WS · **FIX** | positions · fills | ✅ |
| **bitmex** | perp · spot <sup>[2]</sup> | full | REST | positions · fills | — |
| **htx** | perp · spot <sup>[3]</sup> | full | REST | positions | — |
| **coinbase** | spot | full | REST | — <sup>[4]</sup> | — |
| `sim` | perp | generated | in-process | positions | — |

<sup>**full** means top-of-book, depth and prints all arrive. A pull that is absent is
*stated* rather than answered with an empty list. **Balances** are ticked only where a
live account answered — the rest declare `balance_pull = false` and refuse, which is not
the same as reporting nothing.</sup>

<details>
<summary><b>Where the gaps are, and why</b></summary>

<br>

**[1] binance** — `@aggTrade` and `@markPrice` are acknowledged on this venue and then
send nothing, while `bookTicker` and `trade` flood. Prints come from `@trade`; funding
is pulled from `/fapi/v1/premiumIndex`, one call covering every symbol.

**[2] bitmex spot** — subscribes and stays silent. Seven live pairs with real volume
and not one frame in 25 s. An adapter defect, not a thin market.

**[3] htx spot** — the catalogue interns none of its 600 tradable pairs, so a spot
gateway starts empty and exits.

**[4] coinbase** — Coinbase Exchange lists no perpetuals, and this adapter implements
no account pulls, so there is nothing to reconcile against.

**Balances** are read on the venues marked above and published live into the gateway's
segment; the rest report nothing and say so rather than answering zero. No margin model
is built on them yet, so there is still no free-collateral check. Funding is the published
*rate*, never the charge a venue actually took — no funding has moved the PnL ledger.

</details>

---

## Quick start

```sh
make deps          # dependencies via Conan
make test          # build and run the suite under ASan + UBSan
make demo          # the whole pipeline against the built-in matching engine
```

`make demo` needs **no API keys**. A price-time-priority matching engine implements the
same gateway contract as a real venue, so gatewayd, strategyd and the journal all run
end to end.

Against a real exchange — still without credentials, because market data needs none:

```sh
gatewayd --venue=deribit --market=option --symbol=BTC-28AUG26-40000-C
gatewayd --venue=bybit   --market=spot   --symbol=BTCUSDT
./tools/venue_smoke.sh   # every venue at once — 16/16 expected
```

> [!NOTE]
> Order entry reads credentials from a **reference, never a value** —
> `--api-key=env:BYBIT_API_KEY`, `file:/run/secrets/key`, or
> `exec:op read op://vault/item/field`. No key reaches a config file or a journal.

---

## Layout

| Path | What lives there |
| :-- | :-- |
| `apps/` | `gatewayd` · `strategyd` · `persisterd` · `replay` · `launcher` |
| `libs/domain` | instruments, orders, the order FSM, fixed-point money |
| `libs/venues` | the sixteen adapters, one directory each |
| `libs/shm` | segments, seqlock tables, broadcast and SPSC rings |
| `libs/journal` | the append-only mmap journal and its reader |
| `libs/portfolio` | positions, PnL, targets, reconciliation |
| `libs/oms` | order store, risk gate, id generation |
| `libs/simcore` | the matching engine behind the sim venue |
| `schema/` | [`journal.xml`](schema/journal.xml) (SBE) · [`admin-api.yaml`](schema/admin-api.yaml) (the terminal's contract) |
| `frontend/` | [the terminal](frontend/README.md) — React, types generated from the contract |
| `tests/` `fuzz/` `bench/` | 1413 tests · fuzz targets · microbenchmarks |

---

## Performance

Cross-process SPSC ping-pong over the shm transport. M-series macOS, unpinned — Linux
pinned-core numbers will be published from the production target rather than estimated
here.

<div align="center">

| | round trip | one way |
| :-- | --: | --: |
| **p50** | 125 ns | 62 ns |
| **p99** | 167 ns | 83 ns |
| **p999** | 292 ns | 146 ns |

</div>

Primitive costs, Release, single process:

| seqlock write | seqlock read | SPSC push+pop | broadcast publish |
| :--: | :--: | :--: | :--: |
| ≈ 1.8 ns | ≈ 0.7 ns | ≈ 1.8 ns | ≈ 2.3 ns |

---

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

---

## Status

<table>
<tr><td width="50%" valign="top">

**Working**

- 16 venues streaming live market data
- Balances read from the venue and published live
- Order entry on every one of them
- Options and dated futures on Deribit
- Positions, PnL, append-only ledger
- Reconciliation against the venue's own word
- Postgres persistence, idempotent replay

</td><td width="50%" valign="top">

**Not built yet**

- A margin model built on those balances
- Funding credited from the actual charge
- Conditional and bracket orders
- Execution algorithms
- A backtest harness over recorded data
- `admind` and the terminal the [contract](schema/admin-api.yaml) describes

</td></tr>
</table>

---

## License

**LGPL-3.0.** Full text in [`LICENSE`](LICENSE); [`COPYING`](COPYING) holds the GPL-3.0
text that LGPL-3.0 incorporates by reference.

Chosen deliberately over a permissive licence. A strategy or an application that links
raikiri stays closed if its author wants it closed, while a fork of the framework
itself has to publish its changes. LGPL carries no network clause, so running raikiri
as a hosted service triggers no obligation either way.
