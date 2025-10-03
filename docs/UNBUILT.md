# Shapes the terminal was designed against, and the backend does not serve

Three screens — Scanner, Strategies and Strategy detail — were designed in full
against fixtures and then found to have no endpoint behind them. The fixtures
have been deleted; the screens now say so plainly rather than showing invented
rows.

What follows is the part of those fixtures that was **a design decision rather
than data**. It is written down here because deleting the file would otherwise
delete the reasoning, and it would be re-derived wrongly later.

None of this is implemented. Nothing here is a promise about when it will be.

## A slot is the unit a strategy thinks in

For a hedge a slot holds **two positions — the long and the short that only mean
anything together**. A strategy runs N slots at once, and the slot is the handle
the intent ring, the fills and the reconciler all key on.

This was learned by getting it wrong: it used to be one slot per leg, which made
a two-legged carry look like two independent trades and left a report unable to
say how many trades were on.

The backend has no slot dimension today — `positions` keys on
`(strategy_id, instrument_id)` and nothing above that. Adding one is a
persistence and journal decision, not a UI one.

## What a strategy must record for a report to be possible

Worked backwards from the questions a desk asks at the end of a month, not from
what is convenient to publish:

- **What it earned, decomposed into price, funding and fees separately.** A net
  figure that cannot be decomposed is a figure nobody can act on.
- **Which legs it held**, on which venues, at what size.
- **Every fill, with the liquidity flag and the fee as the venue charged it.**
  Maker and taker differ by an order of magnitude and a blended fee hides which
  side of that the strategy is on. A NEGATIVE FEE IS A REBATE and stays
  negative rather than being folded into a cost.
- **Every funding settlement, with the rate the venue used** — not a rate
  observed on a ticker near the time — and the position it was applied to.
- **The parameters the run was started with.** A P&L without them is a result
  nobody can reproduce or attribute.

The fill half of this exists on the execution path today. The funding half now
does too: `LedgerEntry` carries what the venue actually charged. Parameters and
slots do not.

## Reading an arbitrage board

**Two spreads, and they are not the same number twice.** The funding spread is
what the trade earns per period; the price spread is how far apart the venues
quote the thing, which is what it costs to get in and what has to converge to
get out. A board showing only the first shows carries that are free money right
up until you cross two wide books to take them.

**The route is oriented so the spread is positive.** A scanner exists to say
which way round to put the trade on; a board of negative edges is the same
information written backwards and the reader has to flip every row in their
head. Hold the leg that PAYS, short the leg that CHARGES.

**Depth is the smaller of the two venues' volumes.** A route is only as deep as
its thinner leg.

**Normalise the interval before comparing anything.** okx funds every four hours
and hyperliquid every one where most venues fund every eight; two raw rates side
by side compare different things. A venue that does not state its interval
cannot be normalised at all, and must be left out rather than assumed to be on
the common one.

## What it would take to serve these

- **Scanner, perp·perp:** BUILT. `apps/kowalski` reads the same shared-memory
  tables everything else does, prices every pair once a second and serves the
  board; admind forwards it and answers 503 when the scanner is gone. The
  arithmetic is `libs/scan`, where the funding edge and the price spread stay
  two separate numbers and a leg whose venue states no interval is refused.
- **Scanner, spot·perp and spot·spot:** the same, once spot gateways are
  attached beside the perp ones.
- **Strategies:** needs a registry. admind knows a strategy only as an integer
  `strategy_id` on orders, positions and ledger rows; there is no name, kind or
  status to list, and `/v1/orders` and `/v1/fills` accept no `strategy_id`
  filter, so even counting per strategy is not expressible.
- **Strategy detail:** the above, plus parameters, slots and per-slot fills.
- **Start/Stop:** a control plane. A second producer into the SPSC command rings
  is a decision to be made deliberately, not a POST added quietly.
