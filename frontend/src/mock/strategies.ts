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

/** One side of the position, held in a numbered slot. The slot is the
 *  strategy's own handle for the leg — it is what the intent ring, the fills
 *  and the reconciler all key on, so it belongs in the report. */
export interface StrategyLeg {
  slot: number
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

export interface StrategyDetail {
  name: string
  kind: StrategyKind
  status: string
  since: string
  heldHours: number
  params: StrategyParams
  legs: StrategyLeg[]
  fills: StrategyFill[]
  funding: FundingAccrual[]
}

const CARRY: StrategyDetail = {
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
  legs: [
    { slot: 0, venue: 'bybit', symbol: 'BTCUSDT', side: 'long', qty: 0.1, entry: 79_181.4, mark: 79_410.2, fundingUsd: -4.18, feesUsd: 1.58 },
    { slot: 1, venue: 'okx', symbol: 'BTC-USDT-SWAP', side: 'short', qty: 0.1, entry: 79_204.9, mark: 79_406.3, fundingUsd: 21.44, feesUsd: 1.61 },
  ],
  fills: [
    { at: '08:02:41', slot: 0, venue: 'bybit', symbol: 'BTCUSDT', side: 'buy', price: 79_181.4, qty: 0.06, liquidity: 'maker', fee: 0.0095, feeAsset: 'USDT', orderId: 'a4f21c', execId: '83956771' },
    { at: '08:02:39', slot: 1, venue: 'okx', symbol: 'BTC-USDT-SWAP', side: 'sell', price: 79_204.9, qty: 0.06, liquidity: 'taker', fee: 0.0238, feeAsset: 'USDT', orderId: 'a4f21b', execId: '2201884' },
    { at: '07:44:12', slot: 0, venue: 'bybit', symbol: 'BTCUSDT', side: 'buy', price: 79_120.6, qty: 0.04, liquidity: 'maker', fee: 0.0063, feeAsset: 'USDT', orderId: 'a4f218', execId: '83956702' },
    { at: '07:44:10', slot: 1, venue: 'okx', symbol: 'BTC-USDT-SWAP', side: 'sell', price: 79_149.2, qty: 0.04, liquidity: 'taker', fee: 0.0158, feeAsset: 'USDT', orderId: 'a4f217', execId: '2201871' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 1, venue: 'okx', symbol: 'BTC-USDT-SWAP', rate: 0.000_112, intervalHours: 4, positionQty: -0.1, mark: 79_402.1, amountUsd: 8.89 },
    { at: '2026-08-28 08:00', slot: 0, venue: 'bybit', symbol: 'BTCUSDT', rate: 0.000_133, intervalHours: 8, positionQty: 0.1, mark: 79_398.4, amountUsd: -10.56 },
    { at: '2026-08-28 04:00', slot: 1, venue: 'okx', symbol: 'BTC-USDT-SWAP', rate: 0.000_158, intervalHours: 4, positionQty: -0.1, mark: 79_140.7, amountUsd: 12.55 },
    { at: '2026-08-28 00:00', slot: 0, venue: 'bybit', symbol: 'BTCUSDT', rate: 0.000_089, intervalHours: 8, positionQty: 0.1, mark: 79_012.2, amountUsd: -7.03 },
  ],
}

const MAKER: StrategyDetail = {
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
  legs: [
    { slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', side: 'long', qty: 18, entry: 185.92, mark: 186.41, fundingUsd: -1.12, feesUsd: -3.94 },
  ],
  fills: [
    { at: '09:14:02', slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', side: 'sell', price: 186.44, qty: 12, liquidity: 'maker', fee: -0.0447, feeAsset: 'USDT', orderId: 'b1002f', execId: '55120944' },
    { at: '09:13:48', slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', side: 'buy', price: 186.35, qty: 12, liquidity: 'maker', fee: -0.0447, feeAsset: 'USDT', orderId: 'b1002e', execId: '55120931' },
    { at: '09:13:11', slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', side: 'buy', price: 186.28, qty: 30, liquidity: 'maker', fee: -0.1117, feeAsset: 'USDT', orderId: 'b1002c', execId: '55120902' },
    { at: '09:12:55', slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', side: 'sell', price: 186.41, qty: 30, liquidity: 'taker', fee: 0.3355, feeAsset: 'USDT', orderId: 'b1002b', execId: '55120888' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 0, venue: 'kucoin', symbol: 'SOLUSDTM', rate: 0.000_037, intervalHours: 8, positionQty: 18, mark: 186.2, amountUsd: -1.12 },
  ],
}

const BASIS: StrategyDetail = {
  name: 'basis-eth',
  kind: 'basis',
  status: 'alive',
  since: '2026-08-27 14:05:33',
  heldHours: 21.3,
  params: { entryBp: 3.2, exitBp: 0.8, maxNotionalUsd: 18_000, maxSlippageBp: 4, maxHoldHours: 72 },
  legs: [
    { slot: 0, venue: 'binance', symbol: 'ETHUSDT', side: 'long', qty: 2, entry: 3_118.9, mark: 3_121.4, fundingUsd: -2.41, feesUsd: 0.62 },
    { slot: 1, venue: 'gate', symbol: 'ETH_USDT', side: 'short', qty: 2, entry: 3_126.4, mark: 3_122.8, fundingUsd: -1.08, feesUsd: 0.71 },
  ],
  fills: [
    { at: '07:51:03', slot: 0, venue: 'binance', symbol: 'ETHUSDT', side: 'buy', price: 3_118.9, qty: 2, liquidity: 'maker', fee: 0.0312, feeAsset: 'USDT', orderId: 'a4f220', execId: '83956740' },
    { at: '07:51:01', slot: 1, venue: 'gate', symbol: 'ETH_USDT', side: 'sell', price: 3_126.4, qty: 2, liquidity: 'taker', fee: 0.0375, feeAsset: 'USDT', orderId: 'a4f21f', execId: '77120410' },
  ],
  funding: [
    { at: '2026-08-28 08:00', slot: 0, venue: 'binance', symbol: 'ETHUSDT', rate: 0.000_051, intervalHours: 8, positionQty: 2, mark: 3_120.1, amountUsd: -0.32 },
    { at: '2026-08-28 00:00', slot: 1, venue: 'gate', symbol: 'ETH_USDT', rate: 0.000_072, intervalHours: 8, positionQty: -2, mark: 3_115.8, amountUsd: 0.45 },
  ],
}

export const STRATEGY_DETAIL: Record<string, StrategyDetail> = {
  [CARRY.name]: CARRY,
  [MAKER.name]: MAKER,
  [BASIS.name]: BASIS,
}

/** The decomposition every report starts from.
 *
 *  Realized comes off the fills, funding off the venue's settlements, fees off
 *  the fills' own fee field — a NEGATIVE fee is a maker rebate and is left
 *  negative rather than folded into a "cost", because a quoter that earns its
 *  spread in rebates should be able to see that it does. */
export function decompose(d: StrategyDetail): {
  unrealized: number
  funding: number
  fees: number
  net: number
  makerFills: number
  takerFills: number
} {
  const unrealized = d.legs.reduce(
    (sum, l) => sum + (l.mark - l.entry) * l.qty * (l.side === 'long' ? 1 : -1),
    0,
  )
  const funding = d.funding.reduce((sum, f) => sum + f.amountUsd, 0)
  const fees = d.fills.reduce((sum, f) => sum + f.fee, 0)
  return {
    unrealized,
    funding,
    fees,
    net: unrealized + funding - fees,
    makerFills: d.fills.filter((f) => f.liquidity === 'maker').length,
    takerFills: d.fills.filter((f) => f.liquidity === 'taker').length,
  }
}
