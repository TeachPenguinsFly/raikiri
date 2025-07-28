import { useEffect, useState } from 'react'

/** Stand-in data, so a screen can be designed before its endpoint exists.
 *
 *  THE SEAM IS HERE, at the data layer, and nowhere else. The previous terminal
 *  put a `LIVE_BACKEND` flag inside the screens, so every screen had two code
 *  paths and both had to be kept working forever; the one that was not being
 *  looked at rotted quietly. A screen in this file's world asks for data and
 *  gets some — it never learns where it came from, so there is only ever one
 *  screen to maintain.
 *
 *  Everything below is SHAPED like the contract and is not real: figures move
 *  on a random walk from a fixed seed, so a screenshot is reproducible and
 *  nobody mistakes a number here for a position they hold. */

/** Deterministic: a mock that reshuffles on every reload makes two screenshots
 *  of the same screen impossible to compare. */
function makeRandom(seed: number): () => number {
  let state = seed >>> 0
  return () => {
    state = (state * 1_664_525 + 1_013_904_223) >>> 0
    return state / 0x1_0000_0000
  }
}

const random = makeRandom(0x5eed)

export const VENUES = [
  'bybit',
  'binance',
  'okx',
  'gate',
  'kucoin',
  'bitget',
  'xt',
  'bingx',
] as const

export interface MockGateway {
  account: string
  venue: string
  market: string
  alive: boolean
  instruments: number
}

const GATEWAYS: MockGateway[] = VENUES.map((venue, i) => ({
  account: venue,
  venue,
  market: 'perp',
  // Two of eight not answering, because a header that is always green is a
  // header nobody reads.
  alive: i !== 5 && i !== 7,
  instruments: 400 + Math.floor(random() * 600),
}))

export interface MockListing {
  instrumentId: string
  venue: string
  market: string
  exchangeSymbol: string
  bid: number
  ask: number
  fundingRate: number
  fundingIntervalHours: number
  volume24h: number
}

export interface MockMarket {
  symbol: string
  listings: MockListing[]
}

const BASES = [
  ['BTC', 79_400], ['ETH', 3_120], ['SOL', 186.4], ['XRP', 2.41], ['DOGE', 0.1732],
  ['BNB', 612.5], ['ADA', 0.7412], ['AVAX', 28.61], ['LINK', 17.83], ['TON', 4.912],
  ['SUI', 3.204], ['APT', 8.117], ['ARB', 0.4193], ['OP', 1.284], ['NEAR', 4.771],
  ['INJ', 19.42], ['TIA', 4.038], ['SEI', 0.3117], ['LTC', 92.4], ['ATOM', 5.201],
] as const

/** The venue's own spelling of a pair, which is the reason the canonical name
 *  exists at all. */
function exchangeSymbol(venue: string, base: string): string {
  switch (venue) {
    case 'gate':
    case 'xt':
      return `${base}_USDT`
    case 'okx':
      return `${base}-USDT-SWAP`
    case 'kucoin':
      return `${base === 'BTC' ? 'XBT' : base}USDTM`
    case 'bingx':
      return `${base}-USDT`
    default:
      return `${base}USDT`
  }
}

const MARKETS: MockMarket[] = BASES.map(([base, price], index) => {
  // Not every venue lists everything — a market four venues quote and one that
  // one venue quotes must look different, because that difference is the whole
  // reason for a cross-venue view.
  const listed = VENUES.filter((_, v) => (index + v) % 7 !== 3)
  return {
    symbol: `${base}/USDT`,
    listings: listed.map((venue, v) => {
      const drift = 1 + (random() - 0.5) * 0.0012
      const mid = Number(price) * drift
      const halfSpread = mid * (0.00002 + random() * 0.00012)
      return {
        instrumentId: `${index * 100 + v}`,
        venue,
        market: 'perp',
        exchangeSymbol: exchangeSymbol(venue, String(base)),
        bid: mid - halfSpread,
        ask: mid + halfSpread,
        fundingRate: (random() - 0.45) * 0.0004,
        // okx funds every four hours where the rest fund every eight, and the
        // difference is invisible unless the interval travels with the rate.
        fundingIntervalHours: venue === 'okx' ? 4 : 8,
        volume24h: Math.floor(random() * 900_000_000),
      }
    }),
  }
})

export interface MockBalance {
  account: string
  venue: string
  asset: string
  total: number
  available: number | null
  reported: boolean
}

export const BALANCES: MockBalance[] = [
  { account: 'kucoin', venue: 'kucoin', asset: 'USDT', total: 1_071.377, available: 1_071.377, reported: true },
  { account: 'binance', venue: 'binance', asset: 'USDT', total: 827.04, available: null, reported: true },
  { account: 'bybit', venue: 'bybit', asset: 'USDT', total: 494.106, available: null, reported: true },
  { account: 'bybit', venue: 'bybit', asset: 'XRP', total: 0.001, available: null, reported: true },
  { account: 'xt', venue: 'xt', asset: 'USDT', total: 288.18, available: 286.33, reported: true },
  { account: 'gate', venue: 'gate', asset: 'USDT', total: 191.951, available: 191.951, reported: true },
  { account: 'okx', venue: 'okx', asset: '', total: 0, available: null, reported: false },
  { account: 'bitget', venue: 'bitget', asset: '', total: 0, available: null, reported: false },
]

// ---------------------------------------------------------------- hooks ----

export function useGateways(): MockGateway[] {
  return GATEWAYS
}

export function useMarkets(query: string): MockMarket[] {
  const needle = query.trim().toUpperCase()
  return needle === ''
    ? MARKETS
    : MARKETS.filter((m) => m.symbol.toUpperCase().includes(needle))
}

export function useBalances(): MockBalance[] {
  return BALANCES
}

/** A market whose prices move, so the flash-on-change and the spread column
 *  have something to do. One timer for the whole screen rather than one per
 *  row: twenty rows ticking on twenty timers is twenty renders a second where
 *  one would do. */
export function useTickingMarket(market: MockMarket | null, ms = 1000): MockMarket | null {
  const [tick, setTick] = useState(0)
  useEffect(() => {
    const id = setInterval(() => setTick((n) => n + 1), ms)
    return () => clearInterval(id)
  }, [ms])

  if (market === null) return null
  const walk = makeRandom(0xbeef + tick)
  return {
    ...market,
    listings: market.listings.map((l) => {
      const step = 1 + (walk() - 0.5) * 0.0004
      const mid = ((l.bid + l.ask) / 2) * step
      const half = (l.ask - l.bid) / 2
      return { ...l, bid: mid - half, ask: mid + half }
    }),
  }
}

export interface MockLevel {
  price: number
  size: number
}

/** A ladder that is deeper near the touch and thins out, because a book drawn
 *  from a flat random walk looks like nothing any venue ever sends. */
export function useBook(mid: number | null, levels = 20, ms = 1000): {
  bids: MockLevel[]
  asks: MockLevel[]
} {
  const [tick, setTick] = useState(0)
  useEffect(() => {
    const id = setInterval(() => setTick((n) => n + 1), ms)
    return () => clearInterval(id)
  }, [ms])

  if (mid === null) return { bids: [], asks: [] }
  const walk = makeRandom(0xfeed + tick)
  const step = mid * 0.00008
  const side = (direction: -1 | 1) =>
    Array.from({ length: levels }, (_, i) => ({
      price: mid + direction * step * (i + 1),
      size: Number(((1 + walk() * 6) / (1 + i * 0.12)).toFixed(3)),
    }))
  return { bids: side(-1), asks: side(1) }
}

// ------------------------------------------------------ blotter fixtures ----

export interface MockOrder {
  id: string
  venue: string
  symbol: string
  side: 'buy' | 'sell'
  type: 'limit' | 'market'
  price: number | null
  qty: number
  filled: number
  status: string
  age: string
}

export const ORDERS: MockOrder[] = [
  { id: 'a4f21c', venue: 'bybit', symbol: 'BTCUSDT', side: 'buy', type: 'limit', price: 79_180.0, qty: 0.25, filled: 0.1, status: 'partially_filled', age: '3m 12s' },
  { id: 'a4f220', venue: 'binance', symbol: 'ETHUSDT', side: 'sell', type: 'limit', price: 3_142.5, qty: 4, filled: 0, status: 'accepted', age: '11m 04s' },
  { id: 'a4f21e', venue: 'okx', symbol: 'SOL-USDT-SWAP', side: 'buy', type: 'limit', price: 184.2, qty: 50, filled: 0, status: 'accepted', age: '26m 41s' },
  { id: 'a4f219', venue: 'gate', symbol: 'BTC_USDT', side: 'sell', type: 'limit', price: 79_960.0, qty: 0.4, filled: 0, status: 'pending_cancel', age: '1h 02m' },
]

export interface MockTrade {
  id: string
  venue: string
  symbol: string
  side: 'buy' | 'sell'
  price: number
  qty: number
  fee: number
  liquidity: 'maker' | 'taker'
  at: string
}

export const TRADES: MockTrade[] = [
  { id: '83956771', venue: 'bybit', symbol: 'BTCUSDT', side: 'buy', price: 79_181.4, qty: 0.1, fee: 0.0158, liquidity: 'maker', at: '08:02:41' },
  { id: '83956764', venue: 'kucoin', symbol: 'XBTUSDTM', side: 'sell', price: 79_402.0, qty: 0.05, fee: 0.0119, liquidity: 'taker', at: '07:58:12' },
  { id: '83956740', venue: 'binance', symbol: 'ETHUSDT', side: 'buy', price: 3_118.9, qty: 2, fee: 0.0031, liquidity: 'maker', at: '07:51:03' },
  { id: '83956702', venue: 'xt', symbol: 'BTC_USDT', side: 'buy', price: 79_120.6, qty: 0.02, fee: 0.0009, liquidity: 'maker', at: '07:44:55' },
]

export interface MockPosition {
  /** The interned instrument id. A position has no id of its own — this is
   *  what the book, the journal and the ledger key it by. */
  instrumentId: string
  venue: string
  symbol: string
  qty: number
  entry: number
  mark: number
  /** Null where the venue reports no position endpoint. NULL IS NOT FLAT. */
  venueQty: number | null
}

export const POSITIONS: MockPosition[] = [
  { instrumentId: '6', venue: 'xt', symbol: 'BTC_USDT', qty: 0.02, entry: 79_120.6, mark: 79_419.5, venueQty: 0.02 },
  { instrumentId: '0', venue: 'bybit', symbol: 'BTCUSDT', qty: 0.1, entry: 79_181.4, mark: 79_410.2, venueQty: 0.1 },
  { instrumentId: '101', venue: 'binance', symbol: 'ETHUSDT', qty: 2, entry: 3_118.9, mark: 3_121.4, venueQty: null },
  { instrumentId: '4', venue: 'kucoin', symbol: 'XBTUSDTM', qty: -0.05, entry: 79_402.0, mark: 79_392.4, venueQty: -0.05 },
]

export interface MockStrategy {
  name: string
  kind: string
  status: string
  venues: string[]
  pnl: number
  orders: number
  trades: number
}

export const STRATEGIES: MockStrategy[] = [
  { name: 'funding-carry-btc', kind: 'carry', status: 'alive', venues: ['bybit', 'okx'], pnl: 412.87, orders: 18, trades: 41 },
  { name: 'basis-eth', kind: 'basis', status: 'alive', venues: ['binance', 'gate'], pnl: -63.2, orders: 7, trades: 12 },
  { name: 'maker-sol', kind: 'maker', status: 'stale', venues: ['kucoin'], pnl: 88.41, orders: 0, trades: 156 },
]

// ------------------------------------------------------- series fixtures ----

/** A walk that ends where the caller says it ends.
 *
 *  Generated backwards from the last point, so the series agrees with the
 *  figure printed beside it. A forward walk drifts to wherever it drifts and
 *  the headline number then has to be read off the chart, which is how a
 *  screenshot ends up showing a total that no row sums to. */
function walkTo(end: number, points: number, volatility: number, seed: number): number[] {
  const rand = makeRandom(seed)
  const out = new Array<number>(points)
  let value = end
  for (let i = points - 1; i >= 0; i--) {
    out[i] = value
    value /= 1 + (rand() - 0.48) * volatility
  }
  return out
}

/** Hourly stamps ending at a FIXED instant, not at `Date.now()`.
 *
 *  The axis has to be stable for two screenshots to be comparable, and a mock
 *  that says "now" is a mock whose x-axis changes every reload. */
const k_last = Date.UTC(2026, 7, 28, 12, 0, 0)
export const HOURS: number[] = Array.from(
  { length: 96 },
  (_, i) => k_last - (95 - i) * 3_600_000,
)

const k_equity_now = BALANCES.filter((b) => b.reported && b.asset === 'USDT').reduce(
  (sum, b) => sum + b.total,
  0,
)

export const EQUITY: number[] = walkTo(k_equity_now, HOURS.length, 0.012, 0x1234)

export const STRATEGY_PNL: Record<string, number[]> = Object.fromEntries(
  STRATEGIES.map((s, i) => [s.name, walkTo(s.pnl, HOURS.length, 0.05, 0xa100 + i)]),
)
