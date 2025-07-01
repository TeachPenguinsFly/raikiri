# The raikiri terminal

The terminal for raikiri. One page per thing a person does with the system:
watch a market, work an order, run a strategy, hold options, see what the
account is actually worth.

## What is built

The shell, the generated API layer, and the two screens admind can honestly
answer today.

    npm install
    npm run dev      # proxies /v1 to admind on :8787, /v1/stream to :8788

| route | what it shows | source |
| --- | --- | --- |
| `/` | attached gateways, liveness, instrument counts, database reach | `GET /v1/health` |
| `/treasury` | the ledger split by exchange, and summed forward over time | `GET /v1/pnl`, `/v1/positions` |
| `/instruments` | the whole catalogue, virtualised, filterable | `GET /v1/instruments` |
| `/options` | the chain by underlying, expiry and strike | the same, `kind=option` |
| `/login` | sign-in. Designed, not enforced — admind serves no auth endpoint | — |

## One base under every plot

`src/components/chart/` holds the chrome and the geometry, and nothing else in
the app draws either.

| file | what it owns |
| --- | --- |
| `ChartFrame` | the border, header, legend, measured plot box and empty state. A new plot is a `children({ width, height })` function, not another copy of this. |
| `LinePlot` | multi-series lines and area, gridlines, a right value axis, crosshair with a per-series readout |
| `Donut` | proportions as one ring, hover highlight, centre readout |
| `scale.ts` | measurement, round-number ticks, linear mapping, padded domains |
| `palette.ts` | the colours. A venue is coloured from its NAME, so it is the same colour on every chart and a venue that stops trading does not recolour the ones after it. |

Two rules the plots are built on, both learned the hard way:

- **A chart must be correct before it animates.** The donut grows from a
  `from`-only keyframe towards its own attributes, with no fill mode and no
  stagger — anything that holds the opening frame leaves the ring at zero
  length wherever animation frames do not run.
- **Width is measured synchronously, then observed.** A plot that waits for the
  first `ResizeObserver` callback is a plot that is blank in every context where
  that task is not scheduled.

Charts take doubles; screens do not. A decimal string off the wire becomes a
number only where the result is a pixel or an axis domain — every value that is
*displayed* is formatted from the string it arrived as.

`src/api/types.ts` is **generated** from `../schema/admin-api.yaml` by
`npm run types`, which `npm run build` runs first. It is never edited by hand;
an edit there is a change made in the wrong file.

Seven runtime dependencies against v2's twenty-three, and 84 kB gzipped.

**There is no mock path.** A screen that cannot reach admind says so, with the
server's own words and status. v2 had a flag that swapped screens to invented
numbers when the backend was absent, and both paths had to be maintained
forever; a terminal that shows a number nobody sourced is worse than one that
shows nothing.

Greeks, quotes and books ride the live stream and are not wired yet. The
options chain says so on the screen rather than leaving a blank column that
reads as zero.

---

The rest of this file is the plan the work follows.

## What changes from v2

v2 lives in the Go system (`core/frontend`, React 19 + Vite + Tailwind, 8 859
lines, 23 runtime dependencies) and talks to five services through a
grpc-gateway REST surface plus a bespoke WebSocket. It works. Three things
about it do not survive the move.

**Mock data and live data share a codebase.** `LIVE_BACKEND` decides at runtime
whether a screen shows the venue or a jittered fake, and both paths are
maintained. v3 has no mock path: a screen with nothing behind it says so.
Inventing a number is how a terminal teaches you to trust it and then lies.

**Types are written twice.** `src/types/` describes the domain by hand and
`src/api/` describes the same fields again per endpoint. They drift silently,
because nothing checks them against the server. In v3 the contract is the only
place a field is described, and both sides are generated from it.

**The component layer is mostly wrapper.** Radix primitives wrapped in
shadcn-style components wrapped in app components — three layers to render a
button. A trading terminal needs exactly four hard components (a virtualised
table, a chart, a book ladder, a form) and about a dozen trivial ones.

## Stack

    react  react-dom  react-router
    @tanstack/react-table  @tanstack/react-virtual
    lightweight-charts
    zustand

Eight runtime dependencies against v2's twenty-three. Dev: vite, typescript,
tailwind, openapi-typescript.

Deliberately gone, with the reason:

| dropped | why |
| --- | --- |
| `@radix-ui/*` (8 packages) | dialog, tabs, tooltip, switch, label, separator, slot — each is 20–60 lines written directly, and the wrapper layer above them is larger than the primitives |
| `react-hook-form` + `zod` + `@hookform/resolvers` | the forms are a trade ticket and a config editor; both validate against generated types, and a schema library that duplicates the contract is a second source of truth |
| `class-variance-authority`, `clsx`, `tailwind-merge` | variant plumbing for a component library that is no longer there |
| `cmdk` | the command palette is a filtered list over routes and instruments |
| `lucide-react` | roughly twenty icons are used; inline them as SVG and the icon set stops being a dependency |
| `@tanstack/react-query` | one WebSocket carries everything live, and the handful of cold reads do not need a cache layer with its own lifecycle |

Kept because each is load-bearing: TanStack Table and Virtual carry the blotter
and the market watch, which are thousands of rows that update continuously;
lightweight-charts is a chart nobody should write; zustand is the store the WS
writes into.

## The contract

`schema/admin-api.yaml` — OpenAPI 3.1, hand-written, and the ONLY place a field
is described. It sits beside `schema/journal.xml` because both are contracts
this system publishes and must not break quietly.

    schema/admin-api.yaml
       ├─→ frontend/src/api/types.ts     generated, never edited
       └─→ tests/admind/contract_test    every response validated against it

Live data rides a WebSocket carrying the same types. The rule that makes this
worth doing: a field the server can send is a field the spec names, and CI
fails when it is not.

## admind

A new C++ service, `apps/admind`. Boost.Beast is already a dependency (the
venue sockets are built on it) and `libs/net` already has a fixed-buffer
JsonWriter, so serving HTTP costs no new dependency.

What it reads:

- **live** — the gateway shm segments, discovered by listing
  `<runtime_dir>/shm/gw.*`: catalogue, BBO, depth, positions, exec events.
- **history** — Postgres, which `persisterd` already fills: `orders`,
  `order_events`, `executions`, `pnl_increments`, `funding_observations`,
  `positions`, `recon_breaks`, `connectivity_events`, `latency_samples`.

It is a READER first. Every write — halting an instrument, cancelling an order,
changing a risk limit — needs a path into a running `strategyd`, and that path
does not exist yet: the command rings are SPSC with `strategyd` as the single
producer. Adding a second producer is a design decision, not a detail, and the
first release serves reads while it is made.

**Auth is unresolved.** v2 used Google Bearer tokens through the Go admin
service. admind has no identity story yet, and shipping one that "works for
now" is how a terminal ends up with a static token in a repository. It runs on
localhost until this is answered.

## The options desk

Nothing in raikiri can reach an option today. The domain has carried
`OptionInstrument` with strike, expiry and put/call since the beginning, and
Deribit's adapter already maps them — but `VenueConfig` offers only
`enable_spot` and `enable_perp`, no caps row names options, and `gatewayd
--market=` accepts `spot|perp`. The desk is planned against a live venue, so
the core opens first.

What that takes:

1. `VenueConfig` carries ONE `MarketType` instead of two booleans. The two
   flags can express "both" and "neither", which is why `make_venue` grew
   `TwoMarketsAtOnce` and `NoMarketSelected` to refuse them; one field cannot
   express either, and both error arms go away with it.
2. `VenueCaps` gains `.future` and `.option` beside `.spot` and `.perp`, and
   `make_venue` checks the row for the market actually asked for.
3. `gatewayd --market=` accepts `option` and `future`.
4. Deribit declares `.option = true`. Two sizings need raising with it:
   `k_max_instruments` is 2 048 and BTC options alone are 1 066 live, so BTC
   and ETH together overflow it; the shm catalogue holds 4 096.

**Greeks come from the venue.** Deribit publishes delta, gamma, vega, theta,
rho and three implied vols on its ticker. Computing our own would need a vol
surface, a rate curve and a dividend assumption, and every one of those is a
number we would then have to defend against the venue's. The same reasoning
that settled funding: the venue's word is the one the money moves on.

## Screens

| route | what it is |
| --- | --- |
| `/` | market watch, chart, book, ticket, blotter — as v2, against real data |
| `/options` | new: chain by expiry and strike, venue greeks and IV, position by leg |
| `/scanner` | spot arb, basis and carry, funding — recomputed from the live feed |
| `/strategies` | list and detail, parameters, runs |
| `/treasury` | balances per venue and market, margin utilisation |
| `/settings` | venues, instruments, limits, users |

Dropped for now, as asked: the historical funding page. `funding_observations`
keeps filling, so nothing is lost by not drawing it yet.

## Order of work

1. Options reachable in the core, end to end on Deribit.
2. `schema/admin-api.yaml` covering what the system can honestly answer.
3. `apps/admind` serving it, with contract tests.
4. This frontend against it.

The contract comes second on purpose: it can only describe what the core can
answer, and until options are reachable a spec that names them is fiction.
