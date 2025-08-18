/** The three arbitrage boards, carried over from the previous terminal.
 *
 *  Same shapes and the same reading as before — the boards were right; what
 *  they were drawn in was not. Every row names BOTH venues and the leg each
 *  one is, because a spread with no route is a number you cannot act on.
 *
 *  TWO SPREADS, and they are not the same number twice. The funding spread is
 *  what the trade earns per period; the price spread is how far apart the two
 *  venues quote the thing, which is what it costs to get in and what has to
 *  converge to get out. A board with only the first shows carries that are
 *  free money right up until you cross two wide books to take them. */

export interface FundingRow {
  symbol: string
  longVenue: string
  shortVenue: string
  rateLong: number
  rateShort: number
  predictedLong: number | null
  predictedShort: number | null
  intervalLongH: number
  intervalShortH: number
  priceLong: number
  priceShort: number
  /** The funding edge per eight hours, as a rate. Both venues are normalised
   *  to the same period before subtracting — okx funds every four hours, and
   *  two raw rates compare different things. */
  fundingSpreadPct: number
  /** How far apart the two venues quote the market. This is the entry cost and
   *  the thing that has to converge on the way out. */
  basisBps: number
  premiumPct: number
  bboSpreadPct: number
  /** The SMALLER of the two venues' 24h volumes — a route is only as deep as
   *  its thinner leg. */
  volume24hMinUsd: number
}

export interface SpotPerpRow {
  symbol: string
  spotVenue: string
  perpVenue: string
  spotAsk: number
  perpBid: number
  basisBps: number
  perpRate: number
  predictedRate: number | null
  intervalH: number
  premiumPct: number
  volume24hMinUsd: number
}

export interface SpotRow {
  symbol: string
  buyVenue: string
  sellVenue: string
  buyAsk: number
  sellBid: number
  spreadBps: number
  volume24hMinUsd: number
}

function makeRandom(seed: number): () => number {
  let state = seed >>> 0
  return () => {
    state = (state * 1_664_525 + 1_013_904_223) >>> 0
    return state / 0x1_0000_0000
  }
}

/** The venues the scanner can reach. Sixteen of them is why the filter is a
 *  list you tick rather than a row of chips: twenty chips wrap onto three
 *  lines and push the board off the screen. */
const VENUES = [
  'binance', 'bybit', 'okx', 'gate', 'kucoin', 'bitget', 'xt', 'bingx',
  'mexc', 'htx', 'kraken', 'coinex', 'bitmart', 'phemex', 'bitmex', 'deribit',
  'hyperliquid', 'dydx', 'coinbase', 'bitfinex',
]
const BASES: Array<[string, number]> = [
  ['BTC', 79_400], ['ETH', 3_120], ['SOL', 186.4], ['XRP', 2.41], ['DOGE', 0.1732],
  ['BNB', 612.5], ['ADA', 0.7412], ['AVAX', 28.61], ['LINK', 17.83], ['TON', 4.912],
  ['SUI', 3.204], ['APT', 8.117], ['ARB', 0.4193], ['OP', 1.284], ['NEAR', 4.771],
  ['INJ', 19.42], ['TIA', 4.038], ['SEI', 0.3117], ['LTC', 92.4], ['ATOM', 5.201],
]

const rnd = makeRandom(0xa17b)
const pick = (i: number, k: number) => VENUES[(i * 3 + k) % VENUES.length] as string

export const FUNDING_BOARD: FundingRow[] = BASES.map(([base, px], i) => {
  const a = (rnd() - 0.5) * 0.0006
  const b = (rnd() - 0.5) * 0.0006
  const intervalA = i % 4 === 0 ? 4 : 8
  // THE ROUTE IS ORIENTED SO THE SPREAD IS POSITIVE. A scanner exists to say
  // which way round to put the trade on; a board of negative edges is the same
  // information written backwards, and the reader has to flip every row in
  // their head. Hold the leg that PAYS, short the leg that CHARGES.
  const payer = a < b
  const rateLong = payer ? a : b
  const rateShort = payer ? b : a
  const intervalLongH = payer ? intervalA : 8
  const intervalShortH = payer ? 8 : intervalA
  const spread = (rateShort / intervalShortH) * 8 - (rateLong / intervalLongH) * 8
  const priceLong = px * (1 + (rnd() - 0.5) * 0.0016)
  const priceShort = priceLong * (1 + (rnd() - 0.45) * 0.0022)
  return {
    symbol: `${base}/USDT`,
    longVenue: pick(i, payer ? 0 : 1),
    shortVenue: pick(i, payer ? 1 : 0),
    rateLong,
    rateShort,
    predictedLong: i % 5 === 0 ? null : rateLong * (1 + (rnd() - 0.5) * 0.4),
    predictedShort: i % 7 === 0 ? null : rateShort * (1 + (rnd() - 0.5) * 0.4),
    intervalLongH,
    intervalShortH,
    priceLong,
    priceShort,
    fundingSpreadPct: spread * 100,
    basisBps: ((priceShort - priceLong) / priceLong) * 10_000,
    premiumPct: (rnd() - 0.5) * 0.4,
    bboSpreadPct: rnd() * 0.06,
    volume24hMinUsd: Math.floor(rnd() * 400_000_000) + 4_000_000,
  }
}).sort((x, y) => y.fundingSpreadPct - x.fundingSpreadPct)

export const SPOT_PERP_BOARD: SpotPerpRow[] = BASES.map(([base, px], i) => {
  // Same orientation rule: a basis worth taking is a perp trading OVER the
  // spot, so the generated pair is put that way round.
  const spotAsk = px * (1 + (rnd() - 0.5) * 0.002)
  const perpBid = spotAsk * (1 + rnd() * 0.0035)
  return {
    symbol: `${base}/USDT`,
    spotVenue: pick(i, 2),
    perpVenue: pick(i, 4),
    spotAsk,
    perpBid,
    basisBps: ((perpBid - spotAsk) / spotAsk) * 10_000,
    perpRate: (rnd() - 0.4) * 0.0005,
    predictedRate: i % 6 === 0 ? null : (rnd() - 0.4) * 0.0005,
    intervalH: i % 3 === 0 ? 4 : 8,
    premiumPct: ((perpBid - spotAsk) / spotAsk) * 100,
    volume24hMinUsd: Math.floor(rnd() * 300_000_000) + 2_000_000,
  }
}).sort((x, y) => y.basisBps - x.basisBps)

export const SPOT_BOARD: SpotRow[] = BASES.map(([base, px], i) => {
  const buyAsk = px * (1 + (rnd() - 0.5) * 0.0015)
  const sellBid = buyAsk * (1 + rnd() * 0.002)
  return {
    symbol: `${base}/USDT`,
    buyVenue: pick(i, 5),
    sellVenue: pick(i, 6),
    buyAsk,
    sellBid,
    spreadBps: ((sellBid - buyAsk) / buyAsk) * 10_000,
    volume24hMinUsd: Math.floor(rnd() * 200_000_000) + 1_000_000,
  }
}).sort((a, b) => b.spreadBps - a.spreadBps)

export const SCANNER_VENUES = VENUES
