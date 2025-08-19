/** What a strategy has to record for a report to be possible later.
 *
 *  The shape is chosen by working backwards from the questions a desk asks at
 *  the end of a month, not from what is convenient to publish:
 *
 *    - what did this strategy earn, and how much of it was price, funding and
 *      fees separately — a net figure that cannot be decomposed is a figure
 *      nobody can act on;
 *    - which legs was it holding, on which venues, at what size;
 *    - every fill, with the LIQUIDITY FLAG and the fee as the venue charged
 *      it — maker and taker rebates differ by an order of magnitude and a
 *      blended fee number hides which side of that the strategy is on;
 *    - every funding settlement, with THE RATE THE VENUE USED and the position
 *      it was applied to — not a rate we observed on a ticker near the time;
 *    - the parameters it was running with, because a P&L without them is a
 *      result you cannot reproduce or attribute.
 *
 *  Everything here is a fixture shaped like that contract. The fill fields
 *  already exist on the execution path; the funding fields do not — nothing
 *  publishes a venue's own funding charge yet, and until that lands this table
 *  is the specification for it rather than a view of it. */

export type StrategyKind = 'carry' | 'maker' | 'basis'

/** The configuration a run was started with. A P&L reported without these is
 *  a number that cannot be reproduced. */
export interface StrategyParams {
  /** Carry: enter when the interval-normalised funding spread clears this. */
  entryBp?: number
  exitBp?: number
  /** Maker: how far off the touch each side rests, and how often it requotes. */
  quoteOffsetBp?: number
  requoteMs?: number
  /** Maker: inventory the quoter is allowed to carry, and the skew per unit. */
  maxInventory?: number
  skewBpPerUnit?: number
  /** Shared. */
  maxNotionalUsd: number
  maxSlippageBp: number
  /** Cap on how long one position may be held before it is unwound. */
  maxHoldHours?: number
}

/** One side of a slot. */
export interface StrategyLeg {
  venue: string
  symbol: string
  side: 'long' | 'short'
  qty: number
  entry: number
  mark: number
  /** Signed: negative was paid out. */
  fundingUsd: number
  feesUsd: number
}

export interface StrategyFill {
  /** Ours. Derived from (order, leaves, venue execution id), so a resend keys
   *  the same fill rather than minting a second one. */
  id: string
  at: string
  slot: number
  venue: string
  symbol: string
  side: 'buy' | 'sell'
  price: number
  qty: number
  liquidity: 'maker' | 'taker'
  fee: number
  feeAsset: string
  orderId: string
  /** The venue's, kept beside ours: it is the reconciliation key. */
  execId: string
}

/** One settlement, as the venue booked it. The rate is THEIRS — a rate read
 *  off a ticker near the settlement minute is a different number, and the
 *  difference lands in an append-only ledger. */
export interface FundingAccrual {
  at: string
  slot: number
  venue: string
  symbol: string
  rate: number
  intervalHours: number
  positionQty: number
  mark: number
  /** Signed, as charged. */
  amountUsd: number
}

/** A SLOT IS THE UNIT THE STRATEGY THINKS IN, and for a hedge it holds two
 *  positions — the long and the short that only mean anything together.
 *
 *  It used to be one slot per leg, which made a two-legged carry look like two
 *  independent trades and left the report unable to say how many trades were
 *  on. The slot is the handle: the intent ring, the fills and the reconciler
 *  all key on it, and a strategy runs N of them at once. */
export interface StrategySlot {
  index: number
  long: StrategyLeg | null
  short: StrategyLeg | null
}

export interface StrategyOrder {
  id: string
  slot: number
  venue: string
  symbol: string
  side: 'buy' | 'sell'
  type: 'limit' | 'market'
  price: number
  qty: number
  filled: number
  status: string
  placedAt: string
}

export interface StrategyDetail {
  name: string
  kind: StrategyKind
  status: string
  since: string
  heldHours: number
  params: StrategyParams
  slots: StrategySlot[]
  fills: StrategyFill[]
  funding: FundingAccrual[]
  orders: StrategyOrder[]
  /** Cumulative, hourly, oldest first. Three lines rather than one: a carry
   *  whose funding is climbing while its fees climb faster is a different
   *  animal from one whose legs have drifted, and a single net curve shows
   *  neither. */
  curve: { hours: number[]; net: number[]; funding: number[]; fees: number[] }
}

type Draft = Omit<StrategyDetail, 'curve'>

const CARRY: Draft = {
  name: 'funding-carry-btc',
  kind: 'carry',
  status: 'alive',
  since: '2026-08-26 09:12:04',
  heldHours: 50.4,
  params: {
    entryBp: 1.8,
    exitBp: 0.4,
    maxNotionalUsd: 25_000,
    maxSlippageBp: 3,
    maxHoldHours: 168,
  },
  slots: [
    {
      index: 0,
      long: { venue: 'bybit', symbol: 'BTC/USDT', side: 'long', qty: 0.1, entry: 79_181.4, mark: 79_410.2, fundingUsd: -4.18, feesUsd: 1.58 },
      short: { venue: 'okx', symbol: 'BTC/USDT', side: 'short', qty: 0.1, entry: 79_204.9, mark: 79_406.3, fundingUsd: 21.44, feesUsd: 1.61 },
    },
  ],
  fills: [
    { at: '08:02:41', slot: 0, venue: 'bybit', symbol: 'BTC/USDT', side: 'buy', price: 79_181.4, qty: 0.06, liquidity: 'maker', fee: 0.0095, feeAsset: 'USDT', id: 'a1d5602f', orderId: '7f3a91c4', execId: '83956771' },
    { at: '08:02:39', slot: 0, venue: 'okx', symbol: 'BTC/USDT', side: 'sell', price: 79_204.9, qty: 0.06, liquidity: 'taker', fee: 0.0238, feeAsset: 'USDT', id: '6ef2b703', orderId: '1c05de88', execId: '2201884' },
    { at: '07:44:12', slot: 0, venue: 'bybit', symbol: 'BTC/USDT', side: 'buy', price: 79_120.6, qty: 0.04, liquidity: 'maker', fee: 0.0063, feeAsset: 'USDT', id: '0b3f94ad', orderId: '58e1c072', execId: '83956702' },
    { at: '07:44:10', slot: 0, venue: 'okx', symbol: 'BTC/USDT', side: 'sell', price: 79_149.2, qty: 0.04, liquidity: 'taker', fee: 0.0158, feeAsset: 'USDT', id: 'ca7d1e59', orderId: 'aa93f610', execId: '2201871' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 0, venue: 'okx', symbol: 'BTC/USDT', rate: 0.000_112, intervalHours: 4, positionQty: -0.1, mark: 79_402.1, amountUsd: 8.89 },
    { at: '2026-08-28 08:00', slot: 0, venue: 'bybit', symbol: 'BTC/USDT', rate: 0.000_133, intervalHours: 8, positionQty: 0.1, mark: 79_398.4, amountUsd: -10.56 },
    { at: '2026-08-28 04:00', slot: 0, venue: 'okx', symbol: 'BTC/USDT', rate: 0.000_158, intervalHours: 4, positionQty: -0.1, mark: 79_140.7, amountUsd: 12.55 },
    { at: '2026-08-28 00:00', slot: 0, venue: 'bybit', symbol: 'BTC/USDT', rate: 0.000_089, intervalHours: 8, positionQty: 0.1, mark: 79_012.2, amountUsd: -7.03 },
  ],
  orders: [
    { id: '5a2d0f38', slot: 0, venue: 'bybit', symbol: 'BTC/USDT', side: 'buy', type: 'limit', price: 79_090.0, qty: 0.04, filled: 0, status: 'accepted', placedAt: '2026-08-30 08:04:12' },
    { id: 'ee81b74c', slot: 0, venue: 'okx', symbol: 'BTC/USDT', side: 'sell', type: 'limit', price: 79_520.0, qty: 0.04, filled: 0, status: 'accepted', placedAt: '2026-08-30 08:04:12' },
  ],
}

const MAKER: Draft = {
  name: 'maker-sol',
  kind: 'maker',
  status: 'stale',
  since: '2026-08-27 22:41:19',
  heldHours: 12.7,
  params: {
    quoteOffsetBp: 2.5,
    requoteMs: 400,
    maxInventory: 120,
    skewBpPerUnit: 0.04,
    maxNotionalUsd: 12_000,
    maxSlippageBp: 2,
  },
  slots: [
    {
      index: 0,
      long: { venue: 'kucoin', symbol: 'SOL/USDT', side: 'long', qty: 18, entry: 185.92, mark: 186.41, fundingUsd: -1.12, feesUsd: -3.94 },
      short: null,
    },
  ],
  fills: [
    { at: '09:14:02', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'sell', price: 186.44, qty: 12, liquidity: 'maker', fee: -0.0447, feeAsset: 'USDT', id: 'c47e13b8', orderId: '31b7fa08', execId: '55120944' },
    { at: '09:13:48', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'buy', price: 186.35, qty: 12, liquidity: 'maker', fee: -0.0447, feeAsset: 'USDT', id: '7d0a55f1', orderId: 'e6c40b93', execId: '55120931' },
    { at: '09:13:11', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'buy', price: 186.28, qty: 30, liquidity: 'maker', fee: -0.1117, feeAsset: 'USDT', id: '92fe4c60', orderId: '0f8b21da', execId: '55120902' },
    { at: '09:12:55', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'sell', price: 186.41, qty: 30, liquidity: 'taker', fee: 0.3355, feeAsset: 'USDT', id: '38ab6d24', orderId: 'bd5e7093', execId: '55120888' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', rate: 0.000_037, intervalHours: 8, positionQty: 18, mark: 186.2, amountUsd: -1.12 },
  ],
  orders: [
    { id: '10c9a2fb', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'buy', type: 'limit', price: 186.22, qty: 30, filled: 0, status: 'accepted', placedAt: '2026-08-30 09:14:20' },
    { id: '73f6d509', slot: 0, venue: 'kucoin', symbol: 'SOL/USDT', side: 'sell', type: 'limit', price: 186.51, qty: 30, filled: 0, status: 'accepted', placedAt: '2026-08-30 09:14:20' },
  ],
}

const BASIS: Draft = {
  name: 'basis-eth',
  kind: 'basis',
  status: 'alive',
  since: '2026-08-27 14:05:33',
  heldHours: 21.3,
  params: { entryBp: 3.2, exitBp: 0.8, maxNotionalUsd: 18_000, maxSlippageBp: 4, maxHoldHours: 72 },
  slots: [
    {
      index: 0,
      long: { venue: 'binance', symbol: 'ETH/USDT', side: 'long', qty: 2, entry: 3_118.9, mark: 3_121.4, fundingUsd: -2.41, feesUsd: 0.62 },
      short: { venue: 'gate', symbol: 'ETH/USDT', side: 'short', qty: 2, entry: 3_126.4, mark: 3_122.8, fundingUsd: -1.08, feesUsd: 0.71 },
    },
  ],
  fills: [
    { at: '07:51:03', slot: 0, venue: 'binance', symbol: 'ETH/USDT', side: 'buy', price: 3_118.9, qty: 2, liquidity: 'maker', fee: 0.0312, feeAsset: 'USDT', id: '5b90ea31', orderId: 'b20e4d17', execId: '83956740' },
    { at: '07:51:01', slot: 0, venue: 'gate', symbol: 'ETH/USDT', side: 'sell', price: 3_126.4, qty: 2, liquidity: 'taker', fee: 0.0375, feeAsset: 'USDT', id: 'd8207f46', orderId: '4a9c3e81', execId: '77120410' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 0, venue: 'binance', symbol: 'ETH/USDT', rate: 0.000_051, intervalHours: 8, positionQty: 2, mark: 3_120.1, amountUsd: -0.32 },
    { at: '2026-08-28 00:00', slot: 0, venue: 'gate', symbol: 'ETH/USDT', rate: 0.000_072, intervalHours: 8, positionQty: -2, mark: 3_115.8, amountUsd: 0.45 },
  ],
  orders: [],
}

/** The decomposition every report starts from.
 *
 *  Realized comes off the fills, funding off the venue's settlements, fees off
 *  the fills' own fee field — a NEGATIVE fee is a maker rebate and is left
 *  negative rather than folded into a "cost", because a quoter that earns its
 *  spread in rebates should be able to see that it does. */
export function decompose(d: Draft): {
  unrealized: number
  funding: number
  fees: number
  net: number
  /** Traded notional, both sides. What the strategy actually moved — a fee
   *  number without it says nothing about whether the rate is good. */
  volumeUsd: number
  makerFills: number
  takerFills: number
} {
  const unrealized = d.slots
    .flatMap((s) => [s.long, s.short])
    .filter((l): l is StrategyLeg => l !== null)
    .reduce((sum, l) => sum + (l.mark - l.entry) * l.qty * (l.side === 'long' ? 1 : -1), 0)
  const funding = d.funding.reduce((sum, f) => sum + f.amountUsd, 0)
  const fees = d.fills.reduce((sum, f) => sum + f.fee, 0)
  const volumeUsd = d.fills.reduce((sum, f) => sum + f.price * f.qty, 0)
  return {
    unrealized,
    funding,
    fees,
    volumeUsd,
    net: unrealized + funding - fees,
    makerFills: d.fills.filter((f) => f.liquidity === 'maker').length,
    takerFills: d.fills.filter((f) => f.liquidity === 'taker').length,
  }
}

/** The curve is DERIVED from the decomposition, not authored beside it.
 *
 *  When the two were written by hand the chart ended at one number and the
 *  strip above it printed another, which is the kind of disagreement that
 *  makes a reader stop trusting both. Each line walks from zero to the figure
 *  the strip shows; the net line carries a little noise because a P&L that
 *  climbs in a straight line is a P&L nobody believes. */
function withCurve(d: Draft, seed: number): StrategyDetail {
  const p = decompose(d)
  const n = 48
  let state = seed >>> 0
  const jitter = () => {
    state = (state * 1_664_525 + 1_013_904_223) >>> 0
    return state / 0x1_0000_0000 - 0.5
  }
  // A fixed instant, not `now`: two screenshots of one screen have to be
  // comparable, and an axis anchored to the clock changes on every reload.
  const last = Date.UTC(2026, 7, 30, 9, 0, 0)
  const hours = Array.from({ length: n }, (_, i) => last - (n - 1 - i) * 3_600_000)
  const ramp = (end: number, noise: number) =>
    Array.from({ length: n }, (_, i) => {
      const t = i / (n - 1)
      return end * t + jitter() * Math.abs(end) * noise
    })
  return {
    ...d,
    curve: {
      hours,
      net: ramp(p.net, 0.16),
      funding: ramp(p.funding, 0),
      fees: ramp(-p.fees, 0),
    },
  }
}

export const STRATEGY_DETAIL: Record<string, StrategyDetail> = {
  [CARRY.name]: withCurve(CARRY, 0x1234),
  [MAKER.name]: withCurve(MAKER, 0x5678),
  [BASIS.name]: withCurve(BASIS, 0x9abc),
}

