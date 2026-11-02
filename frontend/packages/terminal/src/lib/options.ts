import { num } from '@raikiri/ui'
import type { OptionChain, OptionQuote } from '../api/client'
import type { components } from '../api/types'

type Quote = components['schemas']['Quote']
type Greeks = components['schemas']['Greeks']

/** one venue's listing with the stream's latest figures laid over the snapshot; numbers for geometry and sorting */
export interface Leg {
  venue: string
  id: string
  symbol: string
  unit: OptionQuote['premium_unit']
  /** what a coin premium turns into usd at */
  rate: number | null
  bid: number | null
  ask: number | null
  bidSize: number | null
  askSize: number | null
  mark: number | null
  bidUsd: number | null
  askUsd: number | null
  markUsd: number | null
  bidIv: number | null
  askIv: number | null
  markIv: number | null
  ivModel: boolean
  delta: number | null
  gamma: number | null
  vega: number | null
  theta: number | null
  greeksModel: boolean
  oi: number | null
}

export interface Side {
  legs: Leg[]
  bestBid: Leg | null
  bestAsk: Leg | null
}

export interface Row {
  strike: number
  call: Side
  put: Side
}

export interface LiveChain {
  forward: number | null
  atm: number | null
  rows: Row[]
}

const usd = (price: number | null, unit: Leg['unit'], rate: number | null) =>
  price === null ? null : unit === 'base' ? (rate === null ? null : price * rate) : price

/** a live figure replaces the snapshot's only where the snapshot took it from the venue too */
function leg(
  q: OptionQuote,
  rate: number | null,
  quote: Quote | undefined,
  greeks: Greeks | undefined,
): Leg {
  const bid = quote !== undefined ? num(quote.bid) : num(q.bid)
  const ask = quote !== undefined ? num(quote.ask) : num(q.ask)
  const venueIv = q.iv_source === 'venue' && greeks !== undefined
  const venueGreeks = q.greeks_source === 'venue' && greeks !== undefined
  const mark = (venueIv ? num(greeks.mark_price) : null) ?? num(q.mark)
  return {
    venue: q.venue,
    id: q.instrument_id,
    symbol: q.symbol,
    unit: q.premium_unit,
    rate,
    bid,
    ask,
    bidSize: quote !== undefined ? num(quote.bid_size) : num(q.bid_size),
    askSize: quote !== undefined ? num(quote.ask_size) : num(q.ask_size),
    mark,
    bidUsd: usd(bid, q.premium_unit, rate),
    askUsd: usd(ask, q.premium_unit, rate),
    markUsd: usd(mark, q.premium_unit, rate),
    bidIv: (venueIv ? num(greeks.bid_iv) : null) ?? num(q.bid_iv),
    askIv: (venueIv ? num(greeks.ask_iv) : null) ?? num(q.ask_iv),
    markIv: (venueIv ? num(greeks.mark_iv) : null) ?? num(q.mark_iv),
    ivModel: q.iv_source === 'model',
    delta: (venueGreeks ? num(greeks.delta) : null) ?? num(q.delta),
    gamma: (venueGreeks ? num(greeks.gamma) : null) ?? num(q.gamma),
    vega: (venueGreeks ? num(greeks.vega) : null) ?? num(q.vega),
    theta: (venueGreeks ? num(greeks.theta) : null) ?? num(q.theta),
    greeksModel: q.greeks_source === 'model',
    oi: num(q.open_interest),
  }
}

function side(legs: Leg[]): Side {
  let bestBid: Leg | null = null
  let bestAsk: Leg | null = null
  for (const l of legs) {
    if (
      l.bidUsd !== null &&
      l.bidUsd > 0 &&
      (bestBid === null || l.bidUsd > (bestBid.bidUsd as number))
    )
      bestBid = l
    if (
      l.askUsd !== null &&
      l.askUsd > 0 &&
      (bestAsk === null || l.askUsd < (bestAsk.askUsd as number))
    )
      bestAsk = l
  }
  return { legs, bestBid, bestAsk }
}

export interface Crossing {
  sell: string
  buy: string
  /** return on the premium paid, before fees */
  pct: number
}

/** the best bid on one venue above the best ask on another: both sides can be taken at a profit before fees */
export function crossing(s: Side): Crossing | null {
  const { bestBid: b, bestAsk: a } = s
  if (b === null || a === null || b.venue === a.venue) return null
  const bid = b.bidUsd as number
  const ask = a.askUsd as number
  return bid > ask ? { sell: b.venue, buy: a.venue, pct: ((bid - ask) / ask) * 100 } : null
}

export function instrumentsOf(chain: OptionChain): string[] {
  return chain.rows.flatMap((r) => [...r.call.venues, ...r.put.venues].map((q) => q.instrument_id))
}

export function liveChain(
  chain: OptionChain,
  quotes: Map<string, Quote>,
  greeks: Map<string, Greeks>,
): LiveChain {
  const forward = num(chain.forward)
  const rates = new Map(
    chain.venues.map((v) => [v.venue, num(v.index) ?? num(v.underlying) ?? forward]),
  )
  const legs = (venues: OptionQuote[]) =>
    side(
      venues.map((q) =>
        leg(
          q,
          rates.get(q.venue) ?? forward,
          quotes.get(q.instrument_id),
          greeks.get(q.instrument_id),
        ),
      ),
    )
  return {
    forward,
    atm: num(chain.atm_strike),
    rows: chain.rows.map((r) => ({
      strike: Number(r.strike),
      call: legs(r.call.venues),
      put: legs(r.put.venues),
    })),
  }
}

export function median(values: Array<number | null>): number | null {
  const kept = values.filter((v): v is number => v !== null).sort((a, b) => a - b)
  if (kept.length === 0) return null
  const half = Math.floor(kept.length / 2)
  return kept.length % 2 === 1
    ? (kept[half] as number)
    : ((kept[half - 1] as number) + (kept[half] as number)) / 2
}

/** the strike row's own figures: best bid and ask across venues, the venues' median mark, summed open interest */
export function summary(s: Side): Leg | null {
  const first = s.legs[0]
  if (first === undefined) return null
  const greeksFrom = s.legs.find((l) => !l.greeksModel && l.delta !== null) ?? null
  const oi = s.legs.reduce<number | null>(
    (sum, l) => (l.oi === null ? sum : (sum ?? 0) + l.oi),
    null,
  )
  return {
    ...first,
    venue: '',
    unit: 'quote',
    bid: s.bestBid?.bidUsd ?? null,
    ask: s.bestAsk?.askUsd ?? null,
    bidUsd: s.bestBid?.bidUsd ?? null,
    askUsd: s.bestAsk?.askUsd ?? null,
    bidSize: s.bestBid?.bidSize ?? null,
    askSize: s.bestAsk?.askSize ?? null,
    bidIv: s.bestBid?.bidIv ?? null,
    askIv: s.bestAsk?.askIv ?? null,
    mark: median(s.legs.map((l) => l.markUsd)),
    markUsd: median(s.legs.map((l) => l.markUsd)),
    markIv: median(s.legs.map((l) => l.markIv)),
    ivModel: s.legs.every((l) => l.ivModel),
    delta: greeksFrom?.delta ?? median(s.legs.map((l) => l.delta)),
    gamma: greeksFrom?.gamma ?? median(s.legs.map((l) => l.gamma)),
    vega: greeksFrom?.vega ?? median(s.legs.map((l) => l.vega)),
    theta: greeksFrom?.theta ?? median(s.legs.map((l) => l.theta)),
    greeksModel: greeksFrom === null,
    oi,
  }
}
