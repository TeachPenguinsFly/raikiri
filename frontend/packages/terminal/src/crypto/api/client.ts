import type { components, operations } from '../../api/types'

type Schema<K extends keyof components['schemas']> = components['schemas'][K]

export type Account = Schema<'Account'>
export type Health = Schema<'Health'>
export type VenueMetrics = Schema<'VenueMetrics'>
export type Market = Schema<'Market'>
export type CandlePage = Schema<'CandlePage'>
export type Instrument = Schema<'Instrument'>
export type Quote = Schema<'Quote'>
export type Book = Schema<'Book'>
export type Ticker = Schema<'Ticker'>
export type Funding = Schema<'Funding'>
export type Position = Schema<'Position'>
export type Order = Schema<'Order'>
export type StreamFrame = Schema<'StreamFrame'>
export type Problem = Schema<'Problem'>
type Orders = operations['listOrders']['responses'][200]['content']['application/json']

/** one bucket of a contract's history: the last sample of each field in it, absent where the venue gave none */
export interface MetricPoint {
  /** the bucket's open, ms */
  t: number
  funding_rate?: string
  mark?: string
  index?: string
  open_interest?: string
}

export interface MetricPage {
  instrument_id: string
  interval: string
  points: MetricPoint[]
}

/** where the market-data api answers: at the root for the terminal on its own, under a prefix inside a larger shell */
export let k_base = ''

export function setApiBase(base: string) {
  k_base = base
}

/** a refusal carrying its rfc 9457 problem body */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly problem: Problem | null,
  ) {
    super(problem?.title ?? `HTTP ${status}`)
    this.name = 'ApiError'
  }
}

async function get<T>(path: string, query: Record<string, string | undefined> = {}): Promise<T> {
  const q = new URLSearchParams()
  for (const [k, v] of Object.entries(query)) if (v !== undefined) q.set(k, v)
  const s = q.toString()
  const r = await fetch(`${k_base}${path}${s === '' ? '' : `?${s}`}`, { headers: { accept: 'application/json' } })
  if (!r.ok) throw new ApiError(r.status, (await r.json().catch(() => null)) as Problem | null)
  return (await r.json()) as T
}

async function instruments(account?: string): Promise<Instrument[]> {
  const out: Instrument[] = []
  let cursor: string | undefined
  do {
    const page = await get<{ items: Instrument[]; next_cursor?: string | null }>('/v1/instruments', {
      limit: '1000',
      account,
      cursor,
    })
    out.push(...page.items)
    cursor = page.next_cursor || undefined
  } while (cursor !== undefined && out.length < 50_000)
  return out
}

/** every account's instruments, a few accounts at a time, handed over as each lands: with every venue on, one walk
 *  through the whole catalogue is forty-odd pages in a row, a minute before the first tick size is known */
async function instrumentsBy(accounts: readonly string[], onBatch: (items: Instrument[]) => void): Promise<void> {
  const queue = [...accounts]
  const worker = async () => {
    for (let account = queue.shift(); account !== undefined; account = queue.shift()) {
      try {
        onBatch(await instruments(account))
      } catch {
        // one venue's catalogue failing leaves its legs on default precision; the rest still land
      }
    }
  }
  await Promise.all(Array.from({ length: 8 }, worker))
}

export const api = {
  instrumentsBy,
  health: () => get<Health>('/v1/health'),
  metrics: () => get<VenueMetrics[]>('/v1/venues/metrics'),
  markets: () => get<Market[]>('/v1/markets'),
  candles: (id: string, interval: string, limit = 1000) =>
    get<CandlePage>('/v1/candles', { instrument_id: id, interval, limit: String(limit) }),
  metricsHistory: (id: string, interval: string, limit = 500) =>
    get<MetricPage>('/v1/metrics/history', { instrument_id: id, interval, limit: String(limit) }),
  instruments,
  positions: () => get<Position[]>('/v1/positions'),
  orders: () => get<Orders>('/v1/orders', { open: 'true' }),
  streamToken: () => get<Schema<'StreamToken'>>('/v1/stream/token'),
}
