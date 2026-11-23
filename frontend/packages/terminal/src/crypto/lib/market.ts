import { useEffect, useMemo, useState } from 'react'
import { compact, n } from '@raikiri/ui/v3'
import { api, ApiError, type Instrument, type Quote } from '../api/client'

export type Query<T> =
  | { state: 'loading' }
  | { state: 'ready'; data: T }
  | { state: 'failed'; status: number | null; title: string }

const k_kept = new Map<string, unknown>()

/** the last answer to each ask stays for the life of the tab, so a page opened again shows it at once */
export function useQuery<T>(key: string, run: () => Promise<T>, refreshMs?: number): Query<T> {
  const [result, setResult] = useState<Query<T>>(() =>
    k_kept.has(key) ? { state: 'ready', data: k_kept.get(key) as T } : { state: 'loading' },
  )
  useEffect(() => {
    let live = true
    const once = () =>
      run().then(
        (data) => {
          k_kept.set(key, data)
          if (live) setResult({ state: 'ready', data })
        },
        (err: unknown) =>
          live &&
          setResult({
            state: 'failed',
            status: err instanceof ApiError ? err.status : null,
            title: err instanceof ApiError ? err.message : 'No answer from the server',
          }),
      )
    void once()
    const timer = refreshMs === undefined ? undefined : setInterval(once, refreshMs)
    return () => {
      live = false
      clearInterval(timer)
    }
  }, [key])
  return result
}

/** one venue's listing of a market */
export interface Leg {
  id: string
  venue: string
  quote: string
  exchange: string
  /** decimals the venue prices in */
  places: number
  tick: number
  /** what one unit of the venue's size is in the base asset */
  unit: number
  /** the venue's 24h turnover in dollars, as admind last ranked it */
  volume: number
  /** whether the venue's tick and contract unit are in yet; until then a size could be contracts read as coins */
  sized: boolean
}

export type Kind = 'perp' | 'spot'

export interface Pair {
  key: string
  /** perpetuals and spot are separate pairs of one market: their prices differ by the basis */
  market: Kind
  base: string
  quote: string
  /** one listing per venue, the venues in k_venue_order */
  legs: Leg[]
  /** the legs' 24h turnover summed */
  volume: number
  /** a listing admind split off for quoting far from the market its ticker names: not that coin */
  stray: boolean
}

export const k_venue_order = ['binance', 'okx', 'hyperliquid', 'bybit', 'bitget', 'gate']
const rank = (v: string) => {
  const i = k_venue_order.indexOf(v)
  return i < 0 ? k_venue_order.length : i
}

/** one hue per venue, far enough apart to tell lines apart on the chart and clear of the candles' green and red */
export const k_venue_color: Record<string, string> = {
  binance: '#f0b90b',
  okx: '#f4f4f5',
  hyperliquid: '#60a5fa',
  bybit: '#fb923c',
  bitget: '#22d3ee',
  gate: '#a78bfa',
  deribit: '#a3e635',
}
// the rest by a hash of the name, from hues the named venues leave free
const k_spare = ['#f472b6', '#facc15', '#2dd4bf', '#c084fc', '#fca5a5', '#93c5fd', '#fdba74', '#d9f99d']
export const venueColor = (v: string) =>
  k_venue_color[v] ?? k_spare[[...v].reduce((h, c) => (h * 31 + c.charCodeAt(0)) >>> 0, 7) % k_spare.length]!

/** the published taker rate at the base tier, in basis points */
const k_taker_bp: Record<string, number> = { binance: 5, okx: 5, hyperliquid: 4.5, bybit: 5.5, bitget: 6, gate: 5 }
export const takerBp = (v: string) => k_taker_bp[v] ?? 5

function decimals(tick: string): number {
  const e = /e-(\d+)$/.exec(tick)
  if (e !== null) return Number(e[1])
  const i = tick.indexOf('.')
  return i < 0 ? 0 : tick.replace(/0+$/, '').length - i - 1
}

const k_dollars = new Set(['USDT', 'USDC', 'USD'])

function pairsOf(markets: Awaited<ReturnType<typeof api.markets>>, byId: Map<string, Instrument>): Pair[] {
  const out: Pair[] = []
  for (const m of markets) {
    for (const kind of ['perp', 'spot'] as const) {
      const legs = new Map<string, Leg>()
      // a venue listing the market against both dollars keeps its tether one, the deeper of the two
      for (const l of [...m.listings].sort(
        (a, b) => Number(b.local_symbol.endsWith('USDT')) - Number(a.local_symbol.endsWith('USDT')),
      )) {
        if (l.market !== kind || legs.has(l.venue)) continue
        // spot is quoted in every local currency; the terminal compares dollar books only
        if (kind === 'spot' && !k_dollars.has(l.local_symbol.split('/')[1] ?? '')) continue
        const ins = byId.get(l.instrument_id)
        legs.set(l.venue, {
          id: l.instrument_id,
          venue: l.venue,
          quote: l.local_symbol.split('/')[1] ?? '',
          exchange: l.exchange_symbol,
          places: Math.min(8, decimals(ins?.tick_size ?? '0.01')),
          tick: n(ins?.tick_size) ?? 0.01,
          unit: n(ins?.contract_value) ?? 1,
          volume: l.volume_24h_usd ?? 0,
          sized: ins !== undefined,
        })
      }
      if (legs.size === 0) continue
      const sorted = [...legs.values()].sort((a, b) => rank(a.venue) - rank(b.venue) || b.volume - a.volume)
      out.push({
        // a perpetual keeps the market's own key, so a market picked before spot came along is still found
        key: kind === 'perp' ? m.key : `${m.key}:spot`,
        market: kind,
        base: m.symbol.split('/')[0] ?? m.symbol,
        quote: sorted[0]?.quote ?? '',
        legs: sorted,
        volume: sorted.reduce((a, l) => a + l.volume, 0),
        stray: m.key.includes('@'),
      })
    }
  }
  return out
}

export interface Catalog {
  pairs: Pair[]
  byKey: Map<string, Pair>
  /** every leg by instrument id, with its pair */
  byId: Map<string, { pair: Pair; leg: Leg }>
}

function catalogOf(markets: Awaited<ReturnType<typeof api.markets>>, instruments: Map<string, Instrument>): Catalog {
  const pairs = pairsOf(markets, instruments)
  return {
    pairs,
    byKey: new Map(pairs.map((p) => [p.key, p])),
    byId: new Map(pairs.flatMap((pair) => pair.legs.map((leg) => [leg.id, { pair, leg }] as const))),
  }
}

// kept across mounts like the queries are: the catalogue is not walked twice in one tab
const k_instruments = new Map<string, Instrument>()
let k_instruments_asked = false

/** The markets alone make the list and every venue's leg, so the terminal opens on them. Tick sizes and contract
 *  units come with the instruments, an account at a time, and sharpen the legs as they land. */
export function useCatalog(): Query<Catalog> {
  const markets = useQuery('markets', () => api.markets(), 300_000)
  const [landed, setLanded] = useState(0)
  const list = markets.state === 'ready' ? markets.data : undefined
  useEffect(() => {
    if (list === undefined || k_instruments_asked) return
    k_instruments_asked = true
    // the chosen venues first: their legs are the ones on screen
    const accounts = [...new Set(list.flatMap((m) => m.listings.map((l) => l.account)))].sort(
      (a, b) => rank(a.split('-')[0] ?? a) - rank(b.split('-')[0] ?? b),
    )
    let due: ReturnType<typeof setTimeout> | undefined
    void api.instrumentsBy(accounts, (items) => {
      for (const i of items) k_instruments.set(i.instrument_id, i)
      // forty batches in ten seconds would rebuild the catalogue forty times; once a second is enough
      due ??= setTimeout(() => {
        due = undefined
        setLanded(k_instruments.size)
      }, 1000)
    })
  }, [list])
  return useMemo(
    () => (markets.state === 'ready' ? { state: 'ready', data: catalogOf(markets.data, k_instruments) } : markets),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [markets, landed],
  )
}

/** the pairs as admind ranks markets: those on two venues or more first, each group by 24h turnover. The volumes
 *  come with the catalogue, so ranking costs no subscription to every instrument's ticker */
export function useRanked(catalog: Catalog | undefined): Array<{ pair: Pair; volume: number }> {
  return useMemo(() => {
    if (catalog === undefined) return []
    return catalog.pairs
      .map((pair) => ({ pair, volume: pair.volume }))
      .sort((a, b) => Number(b.pair.legs.length > 1) - Number(a.pair.legs.length > 1) || b.volume - a.volume)
  }, [catalog])
}

export interface Touch {
  bid: number
  ask: number
  bidSize: number
  askSize: number
  mid: number
  /** ms since the venue stamped it */
  ts: number
}

export function touchOf(q: Quote | undefined, leg: Leg): Touch | undefined {
  const bid = n(q?.bid)
  const ask = n(q?.ask)
  if (q === undefined || bid === undefined || ask === undefined || bid <= 0 || ask <= 0) return undefined
  return {
    bid,
    ask,
    bidSize: (n(q.bid_size) ?? 0) * leg.unit,
    askSize: (n(q.ask_size) ?? 0) * leg.unit,
    mid: (bid + ask) / 2,
    ts: Number(q.ts_event) / 1e6,
  }
}

export const spreadBp = (t: Touch | undefined) => (t === undefined ? undefined : ((t.ask - t.bid) / t.mid) * 1e4)

export interface Cross {
  pair: Pair
  /** where selling pays the most and where buying costs the least */
  sell: { leg: Leg; px: number }
  buy: { leg: Leg; px: number }
  /** the best bid over the best ask across venues, in bp of the mid: above zero the venues cross */
  bp: number
  mids: Array<{ leg: Leg; mid: number }>
}

/** the best bid and the best ask of a pair taken from different venues */
export function crossOf(pair: Pair, quotes: Map<string, Quote>): Cross | undefined {
  const now = Date.now()
  const touches = pair.legs.flatMap((leg) => {
    const t = touchOf(quotes.get(leg.id), leg)
    // a quote that stopped moving a minute ago crosses nothing anyone could take
    return t === undefined || now - t.ts > k_stale_ms ? [] : [{ leg, t }]
  })
  let best: { sell: (typeof touches)[number]; buy: (typeof touches)[number]; gap: number } | undefined
  for (const s of touches) {
    for (const b of touches) {
      if (s === b) continue
      const gap = (s.t.bid - b.t.ask) / ((s.t.mid + b.t.mid) / 2)
      if (best === undefined || gap > best.gap) best = { sell: s, buy: b, gap }
    }
  }
  if (best === undefined) return undefined
  const { sell, buy } = best
  const mid = (sell.t.mid + buy.t.mid) / 2
  return {
    pair,
    sell: { leg: sell.leg, px: sell.t.bid },
    buy: { leg: buy.leg, px: buy.t.ask },
    bp: ((sell.t.bid - buy.t.ask) / mid) * 1e4,
    mids: touches.map((x) => ({ leg: x.leg, mid: x.t.mid })),
  }
}

const k_stale_ms = 60_000

/** dollars with a letter for the size, into the billions */
export function usd(v: number | undefined): string {
  if (v === undefined || !Number.isFinite(v)) return '—'
  const a = Math.abs(v)
  return a >= 1e9 ? `$${(v / 1e9).toFixed(2)}B` : compact(v, true)
}

export const px = (v: number | undefined, places: number) =>
  v === undefined ? '—' : v.toLocaleString('en-US', { minimumFractionDigits: places, maximumFractionDigits: places })

export const bp = (v: number | undefined, places = 1) => (v === undefined ? '—' : `${v.toFixed(places)} bp`)

/** a size in the base asset, with fewer decimals the bigger it is */
export function qty(v: number | undefined): string {
  if (v === undefined) return '—'
  const a = Math.abs(v)
  const places = a >= 1000 ? 0 : a >= 10 ? 2 : a >= 1 ? 3 : 4
  return v.toLocaleString('en-US', { minimumFractionDigits: places, maximumFractionDigits: places })
}
