<div align="center">

# raikiri

**A low-latency, multi-process crypto trading framework in C++23**

[![License](https://img.shields.io/badge/license-LGPL--3.0-1f6feb?style=flat-square)](LICENSE)
[![Latency](https://img.shields.io/badge/cross--process%20p50-167%20ns-2da44e?style=flat-square)](#performance)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-1f6feb?style=flat-square)](CMakeLists.txt)
[![Venues](https://img.shields.io/badge/venues-18%20live-2da44e?style=flat-square)](#venues)
[![Markets](https://img.shields.io/badge/markets-spot%20·%20perp%20·%20futures%20·%20options-2da44e?style=flat-square)](#venues)
[![Tests](https://img.shields.io/badge/tests-1755-2da44e?style=flat-square)](#build)
[![Sanitizers](https://img.shields.io/badge/ASan%20·%20UBSan%20·%20TSan-green?style=flat-square)](#build)

[Venues](#venues) · [Architecture](#architecture) · [Quick start](#quick-start) ·
[Performance](#performance) · [API contract](schema/admin-api.yaml) · [Status](#status)

</div>

---

## Venues

Generated from [`libs/exec/src/caps.cpp`](libs/exec/src/caps.cpp) by
[`tools/venue_table.py`](tools/venue_table.py) — **what the venue serves and what this
adapter reaches, never what a strategy should trust.**

<!-- venues:begin -->
| venue | custody | markets | order entry | WS | FIX | amend | private stream | funding ledger |
| :-- | :-: | :-- | :-- | :-: | :-: | :-: | :-- | :-: |
| `aster` | DEX | perp | **market data only** | – | – | – | – | – |
| `binance` | CEX | perp | REST + WS | ✓ | ✓ | ✓ | ListenKey | ✓ |
| `bingx` | CEX | spot · perp | REST | – | – | – | ListenKey | ✓ |
| `bitfinex` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `bitget` | CEX | spot · perp | REST + WS | ✓ | – | – | AuthSubscribe | ✓ |
| `bitmex` | CEX | spot · perp | REST | – | – | ✓ | AuthSubscribe | – |
| `bybit` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `coinbase` | CEX | spot | REST | – | – | – | AuthSubscribe | – |
| `deribit` | CEX | perp · futures · options | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `gate` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `htx` | CEX | spot · perp | REST | – | – | – | AuthSubscribe | ✓ |
| `hyperliquid` | DEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `kraken` | CEX | perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | – |
| `kucoin` | CEX | spot · perp | REST + WS | ✓ | – | – | ListenKey | – |
| `mexc` | CEX | spot · perp | REST | – | – | – | AuthSubscribe | ✓ |
| `okx` | CEX | spot · perp | REST + WS | ✓ | – | ✓ | AuthSubscribe | ✓ |
| `weex` | CEX | perp | REST | – | – | – | – | ✓ |
| `xt` | CEX | spot · perp | REST | – | – | ✓ | ListenKey | ✓ |
<!-- venues:end -->

`order entry` is *market data only* when the adapter sends nothing at all — for
`aster` that is not an omission but a different signer: it authorises with an EVM
wallet signature rather than a key/secret pair, so no credential this codebase carries
would work.

`custody` is the one column the caps table cannot answer: no capability bit separates
a venue holding your coins from a contract that does not. It lives in the generator,
which refuses to build rather than default a venue it has no entry for.

`private stream` names how the venue authenticates its account socket. `ListenKey`
mints a URL-bearing token over REST; `AuthSubscribe` signs a frame on an already-open
socket; `—` means the venue publishes no account socket at all, so a fill reaches the
process through the reconciliation pull and nowhere else.

`funding ledger` is what the venue **charged**, read from its own ledger — never
derived from the public rate. An adapter that cannot ask and one that was charged
nothing are different facts, and this column separates them.

<details>
<summary><b>Adding a venue</b></summary>

<br>

A market-data adapter is roughly 3,500 lines: catalogue, symbol map, book, envelope
dispatch, REST builder and the websocket client. Order entry adds a signer, an
execution client and the reconciliation pulls.

The order of work that has held up: capture fixtures from the live venue first
(`tests/venues/<name>/fixtures`, with a README recording provenance), write the
parsers against those bytes, then wire the client. A parser test is only evidence if
the bytes are the bytes the venue actually sends — which is why every fixture
directory names the endpoint and the date it was captured.

Register in four places: `VenueId` in `libs/domain`, the caps row, the factory, and
`register_adapter()` in `apps/gatewayd/main.cpp`. Registration is explicit on purpose —
static self-registration lets the linker silently drop a venue from a static library,
and a venue that vanishes at link time without a diagnostic is exactly the class of
magic this codebase refuses.

</details>

---

## Quick start

No credentials are needed to watch a market — every adapter's public path runs
unauthenticated.

```sh
make deps && make build

# one venue, one symbol, twenty seconds of live market data
build/dev-asan/apps/gatewayd/gatewayd --venue=bybit --symbol=BTCUSDT --duration 20

# every implemented venue at once, reporting what each actually delivered
tools/venue_smoke.sh 20
```

A gateway prints what it received when it stops — instruments loaded, then BBO, depth,
trade and funding counts. **Silence is the failure mode this reports against:** a venue
can rename a channel or stop publishing a stream without any unit test noticing, so the
smoke run fails a venue that loads no instruments or publishes no quote.

Credentials are passed as **references, never values** — `env:NAME`, `file:/path`,
`exec:command` or `op://vault/item/field`. A secret on a command line is visible to
every process on the box through `ps`.

```sh
gatewayd --venue=bybit --market=perp \
         --api-key=env:BYBIT_KEY --api-secret=op://trading/bybit/secret
```

---

## Performance

Measured on an Apple M-series laptop, release build, `make bench`. Linux x86-64 is the
production target; these are the numbers the development machine gives.

**Shared-memory primitives** — single operation, no contention:

| operation         |    time |
| :---------------- | ------: |
| seqlock read      | 0.68 ns |
| SPSC push + pop   | 1.71 ns |
| seqlock write     | 1.72 ns |
| broadcast publish | 2.14 ns |

**Cross-process round trip** — two processes, one SPSC ring each way, 200,000 round
trips:

|       |    RTT | one way |
| :---- | -----: | ------: |
| p50   | 167 ns |   83 ns |
| p90   | 208 ns |  104 ns |
| p99   | 250 ns |  125 ns |
| p99.9 | 292 ns |  146 ns |

The tail matters more than the median here. `max` was 19.5 µs — a scheduler
preemption, not a data-structure cost, and the reason the production target pins cores.

---

## Status

**Usable today:** market data across all eighteen venues, order entry on the seventeen
the table marks, the shared-memory transport, the SBE journals and their Postgres
tail, and the HTTP/WebSocket admin plane behind
[`schema/admin-api.yaml`](schema/admin-api.yaml).

**Honest about the rest.** The caps table carries, per venue and per capability,
whether a bit is set because a path was exercised against a real account or because it
was implemented and never run. Several venues are the latter, and the distinction is
recorded per bit. On `weex`, for instance, every private path answered `401` rather
than `404` — which is how a routed path was told from an invented one — while the bits
that flipped true name the date a real order, position or settlement went through.
Those comments are the real status page, and they are deliberately more pessimistic
than the code.

The API is **not stable**. This is a framework in active development, and a release
that moves a shared-memory layout will say so.

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

| Channel                                         | Kind                            | Why                                                                                                 |
| :---------------------------------------------- | :------------------------------ | :-------------------------------------------------------------------------------------------------- |
| quotes, depth, funding, greeks, venue positions | seqlock table, overwrite-newest | a stale tick is worthless — a reader that falls behind should skip, never queue                     |
| market data, execution events                   | broadcast ring, gap-detecting   | a fill that was dropped is money the book never learned about; an overrun is reported, not absorbed |
| order commands                                  | SPSC ring, lossless             | the escape hatch must work precisely when something is wrong                                        |

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

## License

**LGPL-3.0.** Full text in [`LICENSE`](LICENSE); [`COPYING`](COPYING) holds the GPL-3.0
text that LGPL-3.0 incorporates by reference.

Chosen deliberately over a permissive licence. A strategy or an application that links
raikiri stays closed if its author wants it closed, while a fork of the framework
itself has to publish its changes. LGPL carries no network clause, so running raikiri
as a hosted service triggers no obligation either way.

## Contact Us

contact@tpf-labs.com