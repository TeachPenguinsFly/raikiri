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

const BALANCES: MockBalance[] = [
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
