# raikiri

> Working name — final project name TBD.

A low-latency, multi-process crypto trading framework in C++23. No message broker:
processes communicate over lock-free shared-memory channels; durability and the cold
plane run through per-process append-only journals tailed into PostgreSQL.

**Status: `make demo` works — the full multi-process pipeline runs end-to-end
against the built-in matching engine, no API keys needed.**

```
===== demo: sim venue, 10s, single unpinned core (macOS dev laptop) =====
metric (ns)                count         p50         p90         p99
tick-to-callback           19631        1007        2015        7039
submit-call                  156         295        3391       14335
order-round-trip             156        8703       12543       27647
fills=139  exec-ring-overruns=0  fsm-refusals=0
```

tick-to-callback: BBO written by the gateway process → strategy callback in
the kernel process (cross-process, via seqlock table). submit-call: risk gate
+ order store + journal append + command-ring push. order-round-trip: submit
→ matching-engine verdict delivered back across both process boundaries.

## Design in one paragraph

Each exchange gateway is its own process (`gatewayd --venue=...`) owning a
shared-memory segment: seqlock tables for best-bid/offer state (overwrite-newest),
broadcast rings for market-data and execution events, and lossless SPSC rings for
order commands. A single-threaded strategy kernel (`strategyd`) busy-polls all
segments, routes events through a typed in-process message bus to strategy slots, and
fires orders back through the command rings. Every process appends SBE-encoded records
to its own mmap journal; `persisterd` tails journals into Postgres — the hot path
never touches the database. A simulated venue (price-time-priority matching engine)
implements the same gateway contract, so the whole system runs end-to-end with no API
keys: `make demo`.

- Production target: Linux (x86-64). macOS is a supported dev/demo platform
  (no core pinning / hugepages; the shm transport is file-backed mmap and portable).
- Money is fixed-point `int64` everywhere; `double` is banned from decision code.
- Hot path: `std::expected`, preallocated, zero-alloc steady state. Cold path:
  exceptions, coroutines for connection orchestration.

## Early numbers

Cross-process SPSC ping-pong over the shm transport (M-series macOS dev
machine, unpinned — Linux pinned-core numbers will be published from the
production target):

| metric | round trip | one-way |
|---|---|---|
| p50 | 125 ns | 62 ns |
| p99 | 167 ns | 83 ns |
| p999 | 292 ns | 146 ns |

Primitive costs (Release, single process): seqlock write ≈ 1.8 ns, seqlock
read ≈ 0.7 ns, SPSC push+pop ≈ 1.8 ns, broadcast publish ≈ 2.3 ns.

## Build

Requires: CMake ≥ 3.28, Ninja, Conan 2, a C++23 compiler (GCC 14 / Clang 18 /
AppleClang 17).

```sh
make deps          # conan install (PRESET=dev-asan by default)
make test          # build + run unit tests under ASan/UBSan
make test PRESET=dev-tsan
make bench         # Release benchmarks
```

## License

LGPL-3.0. The full text is in `LICENSE`; `COPYING` holds the GPL-3.0 text that
LGPL-3.0 incorporates by reference.

Chosen deliberately over a permissive licence: a strategy or application that
links raikiri stays closed if its author wants it closed, while a fork of the
framework itself has to publish its changes. LGPL carries no network clause, so
running raikiri as a hosted service triggers no obligation either way.
