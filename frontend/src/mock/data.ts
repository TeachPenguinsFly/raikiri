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
  /** Read off the account name — perp is the unsuffixed default, as
   *  `format_account` writes it. */
  market: 'perp' | 'spot'
  asset: string
  total: number
  available: number | null
  reported: boolean
}

export const BALANCES: MockBalance[] = [
  { account: 'kucoin', venue: 'kucoin', market: 'perp', asset: 'USDT', total: 1_071.377, available: 1_071.377, reported: true },
  { account: 'kucoin-spot', venue: 'kucoin', market: 'spot', asset: 'USDT', total: 214.09, available: 214.09, reported: true },
  { account: 'kucoin-spot', venue: 'kucoin', market: 'spot', asset: 'BTC', total: 0.0041, available: 0.0041, reported: true },
  { account: 'binance', venue: 'binance', market: 'perp', asset: 'USDT', total: 827.04, available: null, reported: true },
  { account: 'bybit', venue: 'bybit', market: 'perp', asset: 'USDT', total: 494.106, available: null, reported: true },
  { account: 'bybit', venue: 'bybit', market: 'perp', asset: 'XRP', total: 0.001, available: null, reported: true },
  { account: 'xt', venue: 'xt', market: 'perp', asset: 'USDT', total: 288.18, available: 286.33, reported: true },
  { account: 'gate', venue: 'gate', market: 'perp', asset: 'USDT', total: 191.951, available: 191.951, reported: true },
  { account: 'okx', venue: 'okx', market: 'perp', asset: '', total: 0, available: null, reported: false },
  { account: 'bitget', venue: 'bitget', market: 'perp', asset: '', total: 0, available: null, reported: false },
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

/** THE CANONICAL SYMBOL IS WHAT THE SCREEN SHOWS.
 *
 *  `BTC_USDT`, `XBTUSDTM` and `BTC-USDT-SWAP` are three venues spelling one
 *  market, and a blotter that prints the venue's spelling makes the reader do
 *  the normalisation the domain layer already did. The exchange symbol stays
 *  on the record — it is what you quote at support — but it is not the label.
 *
 *  EVERY RECORD CARRIES AN ID OF OURS, and all of them are the same shape:
 *  eight lowercase hex characters. A sequence number is not an identifier —
 *  `P-1043` says how many positions came before it and nothing else, it
 *  collides the moment two processes mint one, and it cannot be spoken over a
 *  phone without the listener wondering what the P stands for. Eight hex
 *  characters is 32 bits, which is enough to be unique across a desk's
 *  lifetime of records and short enough to read back in one breath.
 *
 *  The three namespaces are separate: an order id is minted when the intent is
 *  written, a fill id is DERIVED from (order, leaves, venue execution id) so a
 *  resend produces the same key, and a position id is minted by the book when
 *  a flat instrument stops being flat. The venue's own execution id stays on
 *  the fill beside ours — it is the reconciliation key and the thing you quote
 *  at support, and it is not ours to replace. */

export interface MockOrder {
  id: string
  venue: string
  market: 'perp' | 'spot'
  symbol: string
  exchangeSymbol: string
  side: 'buy' | 'sell'
  type: 'limit' | 'market'
  price: number | null
  qty: number
  filled: number
  status: string
  /** When it was accepted, in UTC. An age is a number that has to be recomputed
   *  to be compared with anything else in the journal. */
  placedAt: string
}

export const ORDERS: MockOrder[] = [
  { id: '7f3a91c4', venue: 'bybit', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTCUSDT', side: 'buy', type: 'limit', price: 79_180.0, qty: 0.25, filled: 0.1, status: 'partially_filled', placedAt: '2026-08-30 08:19:29' },
  { id: 'b20e4d17', venue: 'binance', market: 'perp', symbol: 'ETH/USDT', exchangeSymbol: 'ETHUSDT', side: 'sell', type: 'limit', price: 3_142.5, qty: 4, filled: 0, status: 'accepted', placedAt: '2026-08-30 08:11:37' },
  { id: '3c8f0a62', venue: 'okx', market: 'perp', symbol: 'SOL/USDT', exchangeSymbol: 'SOL-USDT-SWAP', side: 'buy', type: 'limit', price: 184.2, qty: 50, filled: 0, status: 'accepted', placedAt: '2026-08-30 07:56:00' },
  { id: 'e94b7205', venue: 'gate', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTC_USDT', side: 'sell', type: 'limit', price: 79_960.0, qty: 0.4, filled: 0, status: 'pending_cancel', placedAt: '2026-08-30 07:20:14' },
]

export interface MockTrade {
  /** Ours, derived from the order and the leaves so a resend keys the same. */
  id: string
  /** The venue's. Not shown in the blotter — nobody reads a column of foreign
   *  ids down — but kept on the record, because it is the reconciliation key
   *  and the reference you quote at support. */
  venueExecId: string
  at: string
  venue: string
  market: 'perp' | 'spot'
  symbol: string
  exchangeSymbol: string
  side: 'buy' | 'sell'
  price: number
  qty: number
  fee: number
  liquidity: 'maker' | 'taker'
  /** Funding accrued on the position this fill belongs to, since it opened.
   *  Spot has none and shows nothing rather than a zero. */
  fundingUsd: number | null
}

export const TRADES: MockTrade[] = [
  { id: 'a1d5602f', venueExecId: '83956771', at: '2026-08-30 08:02:41', venue: 'bybit', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTCUSDT', side: 'buy', price: 79_181.4, qty: 0.1, fee: 0.0158, liquidity: 'maker', fundingUsd: -4.18 },
  { id: 'c47e13b8', venueExecId: '55120944', at: '2026-08-30 07:58:12', venue: 'kucoin', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'XBTUSDTM', side: 'sell', price: 79_402.0, qty: 0.05, fee: 0.0119, liquidity: 'taker', fundingUsd: 2.31 },
  { id: '5b90ea31', venueExecId: '83956740', at: '2026-08-30 07:51:03', venue: 'binance', market: 'perp', symbol: 'ETH/USDT', exchangeSymbol: 'ETHUSDT', side: 'buy', price: 3_118.9, qty: 2, fee: 0.0031, liquidity: 'maker', fundingUsd: -2.41 },
  { id: 'd8207f46', venueExecId: '77120410', at: '2026-08-30 07:44:55', venue: 'xt', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTC_USDT', side: 'buy', price: 79_120.6, qty: 0.02, fee: 0.0009, liquidity: 'maker', fundingUsd: 0.87 },
]

export interface MockPosition {
  /** Minted by the book when a flat instrument stops being flat. A venue has
   *  no id for a netted position and the instrument id names the market rather
   *  than the holding, so this is the only handle the position has. */
  id: string
  venue: string
  market: 'perp' | 'spot'
  symbol: string
  exchangeSymbol: string
  qty: number
  entry: number
  mark: number
  leverage: number
  /** Perp only. Spot cannot be liquidated and shows nothing, not a zero. */
  liqPrice: number | null
  fundingUsd: number | null
  /** Null where a person opened it by hand. A strategy's position is closed by
   *  the strategy; a manual one is closed by whoever opened it, which is why
   *  only these carry close controls. */
  strategy: string | null
}

export const POSITIONS: MockPosition[] = [
  { id: '2e6c8b40', venue: 'xt', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTC_USDT', qty: 0.02, entry: 79_120.6, mark: 79_419.5, leverage: 5, liqPrice: 64_318.2, fundingUsd: 0.87, strategy: null },
  { id: '9a41d3e7', venue: 'bybit', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'BTCUSDT', qty: 0.1, entry: 79_181.4, mark: 79_410.2, leverage: 3, liqPrice: 53_902.7, fundingUsd: -4.18, strategy: 'funding-carry-btc' },
  { id: 'f0725ac9', venue: 'binance', market: 'perp', symbol: 'ETH/USDT', exchangeSymbol: 'ETHUSDT', qty: 2, entry: 3_118.9, mark: 3_121.4, leverage: 4, liqPrice: 2_365.1, fundingUsd: -2.41, strategy: 'basis-eth' },
  { id: '4d18e05b', venue: 'kucoin', market: 'perp', symbol: 'BTC/USDT', exchangeSymbol: 'XBTUSDTM', qty: -0.05, entry: 79_402.0, mark: 79_392.4, leverage: 2, liqPrice: 117_884.0, fundingUsd: 2.31, strategy: null },
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
