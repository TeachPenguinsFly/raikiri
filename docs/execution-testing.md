# Executing from the terminal

What has to be true before a person can place an order in the browser, watch it
fill on eight venues, and trust the money afterwards.

The eight are **gate, bybit, xt, kucoin, binance, bingx, okx, bitget** — the set
v1's `config_strategy.yaml` actually traded. Everything below is scoped to them;
the other eight adapters are not in the way, they are just not the target.

Every claim here was checked against the code or measured against a live venue on
2026-08-27. Where something was measured, the number is given.

---

## Where this stands today

Limit and market orders are fully expressed in the domain —
`OrderType{Limit, Market}`, `TimeInForce{Gtc, Ioc, Fok}`, `PostOnly`,
`ReduceOnly` (`libs/domain/include/raikiri/domain/order.hpp:10-19`) — all eight
adapters build both, and the kernel already has `submit`, `cancel`, `cancel_all`
and `set_target`. The order builders are good code with real per-venue mapping
and honest refusals.

**And no order has ever been sent to any of the eight.** Four things are in the
way, in the order they bite:

1. ~~Three of the eight cannot authenticate.~~ **Fixed 2026-08-28.** okx,
   kucoin and bitget sign with a passphrase; `gatewayd` had no flag for one, so
   kucoin's gateway exited and okx's and bitget's started and then rejected
   every order locally.
2. **No shipped binary can place an order.** The only order-emitting strategy is
   hard-gated to the sim venue (`apps/strategyd/main.cpp:735`); against a real
   venue the kernel runs `class Observer final : public strategy::Strategy {}`,
   whose every callback is a no-op.
3. **There is no write path from outside.** `admind` refuses every non-GET with
   `405 read-only` (`libs/admin/src/routes.cpp:171`) — by design, not by
   omission. See [Phase 1](#phase-1--the-write-path).
4. **The terminal has no live client.** `frontend/src/api/client.ts` makes five
   REST calls and opens no WebSocket. There is no blotter and no ticket.

### If you do only five things

1. ~~`--passphrase` on `gatewayd`~~ — **done.** `caps.needs_passphrase` and a
   `MakeError::PassphraseRequired` refusal in `make_venue`, so a gateway that
   cannot sign says so at build instead of rejecting one order at a time.
2. ~~The fee sign, the ledger key, and the BingX flip~~ — **done**, as one
   change; see below.
3. `restore_book` reading every attached catalogue, not `gateways.front()` — the
   bug that fires on the first restart with eight gateways attached.
4. The market in `raikiri.positions`' primary key — spot and perp on one venue
   currently overwrite each other. Does not fire on a perp-only desk.
5. `marketUnit` on bybit spot market buys — the one order path that is silently
   wrong rather than refused. **Spot only**; a perp-only desk never reaches it.

### What the ledger change was, and why it was one change

Three defects turned out to be one: the fee had the wrong sign, the ledger key
collapsed on venues that mint no execution id, and the fee was read from the
event rather than from what the book actually charged. Fixing any one alone
leaves the ledger inconsistent with the book.

- **`fill_identity(order, leaves, venue_exec_id)`** in `libs/domain` is now the
  one answer to "which fill is this". It returns the venue's id when there is
  one and otherwise derives `~<order>.<leaves>` — **derived, never minted**,
  because a resent notification has to produce the identity it produced the
  first time, which is the entire reason an execution id is consulted. `leaves`
  is the discriminator for the same reason it is everywhere else here: it
  strictly decreases across an order's fills, and a resend carries the value it
  already carried. The `~` marks it as ours, since no venue mints one.
- **The executions table stopped dropping fills.** `mapper.cpp` skipped any fill
  whose `exec_id` was empty, so **no BingX perp execution had ever been
  persisted at all**. It now derives the same identity from the journaled
  fields, which keeps it a pure function of the journal.
- **The fee increment now comes from `applied`, not from the event.**
  `apply_fill` returns early on a resent fill and charges the book nothing,
  while `journal_pnl` was still writing `event.fee` — so a resend put a row in
  the ledger that the book did not have. `close_fee + open_fee` sum to the
  venue's fee exactly and are zero on a resend, which `journal_pnl` already
  drops.
- **And it is negated**, so the ledger honours the sign it documents.
- **BingX's fee is flipped on the way in**, like okx's and bitget's. It was the
  one venue of the eight booking every charge as a gain.

Covered by `tests/domain/fill_identity_test.cpp` (8) and a mapper test that
asserts `source_id` — the field the whole ledger's trustworthiness rests on, and
which nothing had asserted.

---

## Phase 0 — before the first order

Nothing here needs a design decision. All of it is required before real money
touches the system.

- [ ] **Set real risk limits.** `strategyd` hardcodes
      `{.max_open_orders = 64, .max_qty_lots = 1000}` at
      `apps/strategyd/main.cpp:708` and leaves `max_long_lots` / `max_short_lots`
      at `INT64_MAX` — the default is explicitly "unlimited so a caller that has
      not decided a limit is not silently given one", and no caller has decided.
      1000 lots is also not a quantity: it is 1 BTC on bybit and something else
      everywhere else.
- [ ] **Add a price band to the risk gate.** `RiskGate::check` refuses a limit
      price only when it is undefined or `<= 0`
      (`libs/oms/include/raikiri/oms/risk.hpp:83`). An order to buy at 1, or at
      1 000 000, passes. For an automated strategy that is tolerable; for a
      human typing into a ticket it is the single most likely way to lose money,
      and the gate is the only place that can refuse it before the venue does.
- [ ] **Make risk limits configurable per instrument, in notional.** Lots are
      not comparable across instruments and a desk thinks in dollars.
- [ ] **Record `min_notional`, `tick_size` and `lot_size` per test instrument**
      from `GET /v1/instruments` and derive the smallest order each venue will
      accept. An order under a venue's minimum is rejected on the wire, which
      looks identical to an adapter bug on the first attempt.
- [x] **`--passphrase` on `gatewayd`.** Done. The refusal lives in
      `make_venue` rather than in the app, so it is testable and applies to
      every consumer: `caps.needs_passphrase` (okx, kucoin, bitget, coinbase)
      plus `MakeError::PassphraseRequired` when a key and secret were given and
      a passphrase was not. A caller that configured no credentials at all is
      asking for market data and is refused nothing. Configuration is now
      validated *before* the factory lookup, because the caps table is compiled
      for every venue and whether a config is wrong does not depend on which
      adapters happen to be linked.
- [ ] **Load credentials as references only** — `--api-key=env:...`,
      `file:...`, `exec:...`. Already supported; the checklist item is that no
      key reaches a config file, a journal or a shell history.
- [ ] **Decide testnet vs. tiny-live per venue.** Only three of the eight have a
      real testnet, so five will be tested with real money at minimum size:

  | venue | testnet | how |
  |---|---|---|
  | gate | yes | `api-testnet.gateapi.io`, `fx-ws-testnet.gateio.ws` |
  | bybit | yes | `api-testnet.bybit.com`, `stream-testnet.bybit.com` |
  | binance | yes | `testnet.binancefuture.com` |
  | okx | demo | same host, `x-simulated-trading` on the request builder |
  | bitget | demo | a separate product/instType, **futures only** |
  | kucoin | **no** | not implemented |
  | xt | **no** | not implemented |
  | bingx | **no** | not implemented |

- [ ] **Fix `restore_book` before attaching more than one gateway.** This is the
      most dangerous item in this document. `restore_book` is called with
      `*gateways.front().ch.catalogue` (`apps/strategyd/main.cpp:727`) and drops
      every position whose instrument is absent from *that one* catalogue
      (`main.cpp:615`). Run `strategyd --gateway=bybit --gateway=okx` and after a
      restart **every position outside the first gateway silently fails to
      restore** — the kernel comes up flat, reconciliation finds the venue
      holding something, and halts. Eight venues means eight gateways, so this
      fires on the first restart of the first real test.
- [ ] **Add the market to `raikiri.positions`' primary key.** It is
      `(strategy_id, venue, exchange_symbol)` (`sql/migrations/0002_positions.sql:49`)
      with no market dimension, and the upsert conflicts on it
      (`libs/persist/src/sink.cpp:163`). A bybit **perp** `BTCUSDT` position and
      a bybit **spot** `BTCUSDT` position are one row overwriting each other.
      Trading both markets on one venue — which is the point of the eight — is
      exactly what triggers it.
- [ ] **Group-commit `strategyd`'s journal.** There is one `flush()` in the
      process, at exit (`apps/strategyd/main.cpp:909`). `gatewayd` commits every
      50 ms (`apps/gatewayd/main.cpp:646`). Tailing readers see appends
      immediately either way, but a machine crash loses every position row and
      every PnL increment since process start.

---

## Phase 1 — the write path

**The constraint.** `cmd_create` and `cmd_cancel` are SPSC rings
(`libs/shm/src/records.cpp`), and `strategyd` is the single producer. `admind`
writing into them directly would be a second producer into a ring whose
correctness argument is that there is exactly one — the ring would corrupt, not
merely race.

**The design that keeps every invariant.** A new SPSC ring in the *other*
direction, owned by a segment `strategyd` creates:

```
terminal ──POST /v1/orders──▶ admind ──intent ring──▶ strategyd ──cmd ring──▶ gatewayd
                                                        │
                                            RiskGate · OrderStore · journal
```

`admind` is the producer of the intent ring, `strategyd` its only consumer, and
`strategyd` remains the only producer of the command rings. Every order — from a
strategy or from a human — goes through the same risk gate, the same order store
and the same journal. An order that reached a venue without passing `RiskGate`
would be the one order nobody could account for afterwards.

- [ ] **A `strategyd`-owned segment.** Nothing in the repository creates one
      today: `shm::SegmentOwner::create` is called only by `gatewayd`
      (`apps/gatewayd/main.cpp:433`). This segment carries the intent ring and,
      later, the book (Phase 2).
- [ ] **An `OrderIntent` record** — instrument, side, type, TIF, flags, price,
      qty, plus the requesting identity and a client-supplied idempotency key.
      The key matters: a browser that retries a POST must not place two orders.
- [ ] **A `ManualDesk` strategy** holding a reserved `strategy_id`, draining the
      intent ring on `on_timer` and calling `ctx.submit()` / `ctx.cancel()`.
      Manual orders then have their own book, their own PnL and their own risk
      limits, separate from any automated strategy — which is what you want the
      first time a human and a strategy trade the same instrument.
- [ ] **`POST /v1/orders`, `DELETE /v1/orders/{id}` in the contract**, with the
      405 replaced. The refusal text in `routes.cpp:171` and the header comment
      in `apps/admind/main.cpp:12` both have to change with it.
- [ ] **Authentication, before the first write endpoint exists.** `admind` binds
      loopback and enforces nothing. A read-only service on loopback is a
      defensible position; a service that can place orders is not. The sign-in
      screen is built (`frontend/src/routes/Login.tsx`) and deliberately gates
      nothing — it is waiting for this.
- [ ] **An order ticket and a blotter in the terminal** — instrument, side,
      type, price, qty, and a working-orders table with cancel.
- [ ] **Give `strategy::Context` an `amend`.** It has `submit`, `cancel`,
      `cancel_all` and `set_target` and no amend, so `CmdKind::Amend` has no
      producer anywhere — `gatewayd` handles a command nothing writes
      (`apps/gatewayd/main.cpp:631`). Five of the eight adapters build an amend
      request that nothing can reach.
- [ ] **Wire the rate limiter.** `RateLimiter` and `InFlightTable` exist in
      `libs/exec/include/raikiri/exec/order_gate.hpp` and are referenced **only
      by their own test**. No adapter and no app instantiates either, and
      `headline_order_rate_per_s` in the caps table is self-described as
      informative. Bybit and bitget allow 10 orders a second; a UI that lets a
      person hold down a button will find that out from the venue.
- [ ] **Decide what a REST timeout means.** The REST client times out at 5 s and
      a timeout becomes a locally synthesised `Rejected`, which retires the
      order in the store — while it may be live on the venue. That is the
      classic way to end up with a position nobody is tracking, and it is
      reachable on all eight because every one of them places orders over REST.
- [ ] **Make a bad FSM transition loud.** `order_events.hpp:9` promises that an
      unlisted `(status, event)` pair is "a venue-protocol violation we want to
      hear about loudly"; the only consumer increments a counter and drops the
      event (`apps/strategyd/main.cpp:801`), printing the total at shutdown. A
      venue that starts sending an event we do not model would go unnoticed for
      a whole session.

---

## Phase 2 — seeing it live

Measured against a live bybit perp gateway, one instrument, subscribed over the
stream:

```
first quotes frame     1.9 ms      81 frames / 4 s   (30 Hz negotiated)
first book frame     172.4 ms      21 frames / 4 s
```

The transport is fast enough and needs no work. What is missing is a client, and
three honesty gaps.

- [ ] **A stream client in the terminal.** None exists. It needs a
      reference-counted subscription registry: a screen subscribes what it shows
      and unsubscribes on unmount, and two screens asking for the same
      instrument share one subscription. React StrictMode double-mounts in dev —
      a naive `useEffect` subscribe leaks one subscription per mount.
- [ ] **Tear down on a hidden tab.** On `visibilitychange`, start a timer; after
      ten minutes hidden, unsubscribe everything and close the socket.
      Resubscribe from the registry on focus. State is coalesced, not queued, so
      nothing is owed on reconnect — the first frame after resubscribing is the
      current value, which is the only value that was ever wanted.
- [ ] **A server-side backstop.** Per-connection caps exist —
      `k_max_feeds = 64`, `k_max_instruments = 512` per subscribe
      (`libs/admin/src/stream.cpp:576-582`) — but there is **no keepalive and no
      idle timeout**. A client that goes away without closing holds its
      subscriptions until TCP notices. Add a WebSocket ping on a slow cadence
      and drop a connection that has not ponged.
- [ ] **Say something when the gateway dies.** Measured: with a subscription
      live, killing the gateway produced **one final frame and then silence** —
      no `problem` frame, no notice. `GET /v1/accounts` correctly reported
      `alive: false` the whole time, but a client watching only the stream sees
      a price that stopped moving, which is indistinguishable from a quiet
      market. The stream should send a problem frame when a subscription's
      gateway owner goes away.
- [ ] **Publish `strategyd`'s book.** The live `positions` feed carries the
      **venue's** half only — `qty` and `venue_qty` are the same number by
      construction (`libs/admin/src/stream.cpp:441`), `avg_entry_price` and
      `realized` are empty, `strategy_id` is always 0, and the underlying cells
      refresh every **30 s** (`apps/gatewayd/main.cpp:225`). So after a partial
      fill the screen shows the pre-fill quantity for up to half a minute, and
      the cost basis never. The book with cost basis lives only in `strategyd`'s
      process memory (`apps/strategyd/main.cpp:287`) and reaches a screen only
      through Postgres. The `strategyd` segment from Phase 1 is where it should
      be published.
- [ ] **Fix the empty decimals.** `Position::avg_entry_price` and
      `Position::realized` are `std::string` in
      `libs/admin/include/raikiri/admin/types.hpp:133,136`, so an unknown value
      serialises as `""`, which does not match the contract's `Decimal` pattern
      and parses to `NaN` in the browser. They should be `std::optional` and
      absent. The same applies to the optional greeks and funding fields.
- [ ] **`database.reachable` on the stream is always false**
      (`libs/admin/src/registry.cpp:103`) regardless of Postgres. The HTTP
      `/v1/health` fills it in; the stream never does.
- [ ] **Expose the trade tape.** `md_events` carries every public print at full
      rate and `Feed` has no value for it — time and sales is published into
      shared memory and no client can read it.

---

## Phase 3 — money you can trust

What already happens on every fill (`apps/strategyd/main.cpp:821-823`): a
`Realized` increment and a `Fee` increment, one per kind per `exec_id`, then the
position row. The ledger is append-only and idempotent, and `/v1/pnl` serves it.
The arithmetic underneath — linear and inverse, flip splitting, basis release on
a full close, a resent fill folding to nothing — is the best-tested code in the
repository.

**The plumbing around it is not.** Five defects below are not gaps to be filled
later; they are wrong answers waiting for the first real fill. None can be caught
by any test that runs today, because `make demo` runs against the sim venue and
**the sim charges no fee** — so not one `kind='fee'` row has ever been produced
by any run of this system, and every fee path below is unexercised.

### The five that produce a wrong number

- [ ] **The fee sign contradicts the ledger's own contract.** The domain treats a
      positive fee as money lost — `net_realized() = realized − fees`
      (`libs/portfolio/src/position.cpp:26`). The contract says the opposite
      about the ledger: *"amount: Signed; positive is money gained"*
      (`schema/admin-api.yaml:796`). `journal_pnl(event, base, Fee, event.fee)`
      (`apps/strategyd/main.cpp:823`) writes the raw positive charge, so a `fee`
      row is a positive number meaning a loss. Any consumer that sums the ledger
      — including this repository's own Treasury page — **adds trading costs to
      profit**, and is wrong by twice the fees.
      *Recommended fix: negate at the writer, so the ledger honours the sign it
      documents. It costs nothing right now precisely because no fee row has ever
      been written; after the first live fill it costs a migration.*
- [ ] **`source_id` collapses when the venue mints no execution id.** It is
      `kind + ":" + venue + ":" + exec_id` (`libs/persist/src/mapper.cpp:161`)
      with `ON CONFLICT (source_id) DO NOTHING`. The *executions* mapper guards
      against an empty id (`mapper.cpp:131`); the PnL mapper does not. **BingX
      perp mints no trade id at all** — deliberately, and asserted by
      `tests/venues/bingx/venue_test.cpp` — so every increment on that venue
      becomes `"fee:bingx:"` / `"realized:bingx:"` and **only the first one ever
      persists**. Gate and KuCoin also tolerate an empty trade id. The key needs
      a fallback, and the market and account belong in it too: `to_string(VenueId)`
      yields `"bybit"`, not `"bybit-spot"`, so spot and perp dedupe against each
      other.
- [ ] **BingX books fees as gains.** Its wire field is documented *"n — negative
      when charged"* (`libs/venues/bingx/include/raikiri/venues/bingx/wire.hpp:355`)
      and `map.cpp:429` assigns it through unflipped, locked in by a test
      expecting `fee.raw == -26'675'760`. OKX and Bitget both flip explicitly,
      with the reason in a comment: *"the ledger this feeds counts a positive
      number as money leaving"* (`libs/venues/okx/src/map.cpp:56`). BingX is the
      one venue of the eight that does not.
- [ ] **KuCoin loses every live fee.** The WS order-change frame names the fee
      *type* and never the amount, so `map.cpp:407` sets `fee = 0` — correctly,
      since undef would poison the arithmetic. The REST fill pull that does carry
      the fee has **no production caller**. On KuCoin every fee is zero until
      fill reconciliation exists.
- [ ] **A fee in a currency that is not the quote is silently mis-booked.**
      `OrderEvent::fee` is one `FixedE9` documented as quote units
      (`libs/domain/include/raikiri/domain/order_events.hpp:98`) with no currency
      beside it. Gate spot sends `"fee":"0.0005","fee_currency":"BTC"` — the
      fixture in this repository has it — and the currency is not parsed, so
      0.0005 BTC is booked as 0.0005 USDT. Binance parses `commissionAsset` into
      its wire struct and then never reads it, so a BNB-discounted fee is booked
      as USDT. OKX, Bitget and KuCoin all publish a fee currency that nothing
      reads. Either the fee carries a currency, or a non-quote fee is refused
      loudly — booking it as quote is the one option that produces a plausible
      wrong number.

### The reconciliation that is advertised but not wired

- [ ] **Fill reconciliation does not exist.** `fill_reports()` is declared,
      implemented by all sixteen adapters, flagged in caps for thirteen of them,
      and called **from tests only** — grep `apps/` and there is not one caller.
      The venue table in `README.md` says "positions · fills" for most venues;
      the fills half is a compiled, tested, entirely unwired function. Same for
      `open_order_reports()` and `query_order()`: `OrderEventType::ReconGap` and
      `ReconClosed` are plumbed through the FSM, the journal enum and the row
      mapper, and **nothing emits either**.
- [ ] **Nothing compares `executions` against `pnl_increments`.** They are
      written by two processes from two journals and never meet, so a fill lost
      to an exec-ring overrun — counted at `apps/strategyd/main.cpp:785`, whose
      comment reads *"policy: halt+reconcile; demo just counts"* — shows up as
      neither.
- [ ] **A reconciliation halt blocks closing as well as opening.**
      `RiskGate::check` refuses on `reconciliation_break` first and
      unconditionally (`libs/oms/include/raikiri/oms/risk.hpp:72`), there is no
      ReduceOnly exemption, and `submit()` is its only caller — so every create
      is gated, not only opening ones as the `Exposure` comment claims. The halt
      then never clears except by restarting the process
      (`apps/strategyd/main.cpp:288`). During a real break you can cancel but you
      cannot flatten, which is the opposite of what a desk needs.
- [ ] **Give `recon_breaks` a verdict column.** It has none, so the verdict is
      recovered by string-matching the English sentence persisterd wrote
      (`libs/admin/src/history.cpp:25`). Two files have to agree on a sentence.

### Still missing, as before

- [ ] **Funding as a charge, not a rate.** All eight publish a rate — four over
      a WebSocket stream (gate, bybit, okx, bitget), one hybrid (kucoin), three
      by REST poll (binance, xt, bingx). **None publishes a charge**, no
      `FundingCharge` type exists, and `pnlKindEnum::Funding` has zero producers.
      This is item 3 of the standing plan, unchanged.
- [ ] **Balances, at all.** No adapter reads an account endpoint; the only hits
      are venues *refusing* a position pull because they hold balances there.
      Gate comes closest — it calls `/futures/{settle}/accounts` and parses only
      the user id out of a response that contains `total`, `available` and
      `unrealised_pnl`. Without balances there is no margin model, no
      free-collateral check, and `portfolio::size_for_risk` — which takes equity
      as an argument — has no caller anywhere outside its own tests.
- [ ] **Surface net-of-fees realized.** `/v1/positions` selects `realized_e9` and
      not `fees_e9`, `cost_e9`, `fill_count` or `ts_opened`, though all four are
      in the table — so realized is gross and average entry is not derivable.
- [ ] **Test the PnL mapper.** `tests/persist/mapper_test.cpp` covers order
      events, executions and positions; its golden journal contains no
      `PnlIncrementMsg` at all, and `source_id` is asserted nowhere except as a
      hand-written literal. The one field the whole ledger's trustworthiness
      rests on is untested.

---

## Phase 4 — the harness

`tools/venue_smoke.sh` proves market data still arrives on every venue. The
execution path needs its counterpart, and it cannot be the same script: it needs
credentials, it moves money, and it must clean up after itself.

- [ ] **`tools/exec_smoke.sh`**, one venue at a time, refusing to run without an
      explicit opt-in variable. Per venue:
      place a limit order far from the market → confirm `Accepted` →
      amend if the venue supports it → cancel → confirm `Canceled` →
      place a marketable order at minimum size → confirm `Filled` →
      confirm one `Realized` and one `Fee` increment in the ledger →
      confirm the position reconciles against the venue's own report →
      close the position → confirm flat on both sides.
- [ ] **Assert the ledger twice.** Replay the journal into Postgres a second time
      and confirm `pnl_increments` is unchanged — the idempotency key is the
      whole reason the ledger can be trusted, and only a double replay tests it.
- [ ] **Run it per market.** Seven of the eight serve spot and perp; a pass on
      perp says nothing about spot, and the two have different minimums,
      different fee schedules and — on binance — different hosts.

---

## The eight, as the code actually is

**Every one of the eight places orders over REST.** Not one uses a WebSocket for
order entry; the private socket in each adapter is the inbound user stream only.
Both order types are mapped by all eight.

| venue | spot | perp | can auth | amend built | cancel by our id | spot market BUY | fills pullable | private stream tested |
|---|:--:|:--:|:--:|:--:|:--:|---|:--:|:--:|
| gate | ✅ | ✅ | ✅ | ✅ | ✅ | refused, stated | ✅ | ✅ |
| bybit | ✅ | ✅ | ✅ | ✅ | ✅ | **silently wrong** | ✅ | ✅ |
| xt | ✅ | ✅ | ✅ | ✅ | ❌ | refused, stated | ✅ | ✅ |
| kucoin | ✅ | ✅ | **❌ exits** | ✅ | ✅ | ✅ | ✅ | ✅ |
| binance | ❌ | ✅ | ✅ | ✅ | ✅ | n/a — no spot | ✅ | ✅ |
| bingx | ✅ | ✅ | ✅ | qty only | ✅ | ✅ | ❌ | ✅ |
| okx | ✅ | ✅ | **❌ rejects** | ✅ | ✅ | ✅ correct | ✅ | ✅ |
| bitget | ✅ | ✅ | **❌ rejects** | ❌ none | ✅ | refused, stated | ✅ | **❌** |

Read the "spot market BUY" column carefully — it is the one place where a venue
difference becomes a wrong order rather than a refusal.

- **gate, xt, bitget refuse it, by design and with a named error.** All three
  size a spot market buy in the *quote* currency and `OrderCmd` has no
  quote-budget field, so the builder returns `SpotMarketBuyNotExpressible`
  rather than guessing. Correct, and the fix is a quote-amount field on the
  command, not a per-venue hack.
- **okx handles it correctly** — it is the only adapter that sends
  `tgtCcy=base_ccy`, with the reasoning written at the line.
- **bybit is silently wrong.** Bybit v5 sizes a spot market buy in the quote
  coin unless `marketUnit:"baseCoin"` is sent; the builder always sends base
  units and `marketUnit` appears nowhere in the tree. A spot market buy on bybit
  is off by roughly the price of the asset. **This is the most dangerous single
  line in the order path**, because it does not refuse — it places.

Other consequences: **xt cannot cancel by our own client order id**, so the order
store must hold the venue's id before a cancel is possible; **bingx cannot pull
fills**, so its reconciliation is position-only; **bitget has no amend at all**
and answers `ModifyRejected`, which its caps row honestly states; and **bitget's
private order path has no test** — the repository holds ten captured private
fixtures that no test loads.

---

## Claims that are not true today

The README opens by promising that *"where a capability is declared but silent on
the wire, this document says so — a framework that reports what it wishes were
true is worse than one that reports nothing."* Four claims currently fail that
standard, and they are in the most-read table in the project.

- [ ] **"Order entry: WS" for bybit, okx, gate, kucoin, bitget** — all five
      place orders over REST. The column renders `caps.ws_order_entry`, which
      `libs/exec/src/caps.cpp:9` explicitly defines as a statement about *the
      venue*, not about this code. The table needs a column that says what the
      adapter does.
- [ ] **"WS · FIX" for binance** — there is no FIX implementation anywhere in
      the repository. Grepping for every FIX protocol marker returns nothing.
- [ ] **"Order entry on every one of them" under Working** — order *builders*
      exist on every one, and no shipped binary can drive them against any real
      venue: the only order-emitting strategy is hard-gated to the sim venue
      (`apps/strategyd/main.cpp:735`), so against a real venue the kernel runs
      `Observer`, whose every callback is a no-op.
- [ ] **Two caps rows understate their own adapter** — kucoin `amend_order =
      false` while spot amend is implemented and wired, and bingx `amend_order =
      false` while a quantity-only perp amend is implemented and wired. A third,
      xt's `cancel_by_client_order_id`, has the right value with a trailing
      comment that contradicts it.

---

## Candles

v1 did not compute bars. It fetched them from each venue's kline REST endpoint
per exchange and forwarded the venue's live kline stream
(`internal/app/gateway/marketdata_spot.go`); there is no candle table in any
migration. That was the right call and it should be the call here too.

**Recommendation: klines come from the venue, and never from a gateway process.**

- A chart is a cold path. A REST fetch on chart load has no business in a process
  that is on the order path — `gatewayd` should stay a thing that does exactly
  one job at line rate.
- The venue's own bars are what a trader compares against. Bars we aggregate
  ourselves will disagree at the edges — a different session boundary, a
  different treatment of the tick that lands on the boundary — and a chart that
  disagrees with the exchange's own chart is worse than no chart.
- It costs nothing. One HTTP call per chart load, cached; the current forming bar
  is built in the browser from the trade stream, which is already published.
- The case for our own bars is real but belongs elsewhere: when the venue's
  history is too short, or sub-second bars are needed, or the bars must be
  reproducible for a backtest. That is the research service's job — it records
  ticks, and any bar can be rebuilt from ticks exactly.

So: `GET /v1/klines?instrument_id=…&interval=…` in `admind`, backed by a small
per-venue kline client, plus a live current-bar built client-side from the trade
tape once that tape is exposed (Phase 2). No storage, no aggregation engine, no
new load on the trading path.

On "institutional grade": that phrase is earned on the order path — a risk gate
that refuses the fat finger, a ledger that survives a replay, a reconciliation
that halts rather than guesses — and not on owning an OHLC pipeline. Every desk
worth the name reads the exchange's own candles.

---

## Order

Phase 0 first, and within it the five items above, because every one of them is
wrong today and silent about it. Then Phase 1, because nothing can be tested
until an order can be placed at all. Phase 2 and Phase 3 proceed in parallel once
orders flow, and Phase 4 is what turns the result from a demonstration into
something that can be re-run after every change.

One note on sequencing that is easy to get wrong: **do the fee sign and the
`source_id` key before the first live fill, not after.** Both are free right now
— no `kind='fee'` row has ever been written by any run of this system, because
`make demo` runs against the sim venue and the sim charges nothing. After the
first real fill they are a migration over a ledger whose whole point is that it
is append-only.

## What this document is not

It does not cover conditional orders, execution algorithms, or a backtest
harness. `OrderType` has two values and that is the right scope for a first live
order — `Triggered` exists in the FSM and bybit maps it inbound, but nothing here
can create a conditional order and nothing should until a market order has filled
correctly on all eight.
