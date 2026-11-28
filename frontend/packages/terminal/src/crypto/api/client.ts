import type { components, operations } from '../../api/types'

/** one schema of the api by name, for screens that keep their calls beside themselves */
export type Schema<K extends keyof components['schemas']> = components['schemas'][K]

export type Account = Schema<'Account'>
export type Health = Schema<'Health'>
export type VenueMetrics = Schema<'VenueMetrics'>
export type Market = Schema<'Market'>
export type CandlePage = Schema<'CandlePage'>
export type Instrument = Schema<'Instrument'>
export type Quote = Schema<'Quote'>
export type Book = Schema<'Book'>
export type Trade = Schema<'Trade'>
export type Ticker = Schema<'Ticker'>
export type Funding = Schema<'Funding'>
export type Position = Schema<'Position'>
export type Order = Schema<'Order'>
export type StreamFrame = Schema<'StreamFrame'>
export type Problem = Schema<'Problem'>
export type Fill = Schema<'Fill'>
export type PnlIncrement = Schema<'PnlIncrement'>
export type PnlKind = Schema<'PnlKind'>
export type ParentOrder = Schema<'ParentOrder'>
export type LeverageState = Schema<'LeverageState'>
export type SetLeverage = Schema<'SetLeverage'>
type Orders = operations['listOrders']['responses'][200]['content']['application/json']
type Fills = operations['listFills']['responses'][200]['content']['application/json']
type Pnl = operations['listPnl']['responses'][200]['content']['application/json']
type Parents = operations['listParentOrders']['responses'][200]['content']['application/json']

/** one page of a history route: a market to narrow to, and where the page before left off */
export interface PageAsk {
  instrument_id?: string | undefined
  cursor?: string | undefined
  limit?: number | undefined
}

const pageOf = (ask: PageAsk): Record<string, string | undefined> => ({
  instrument_id: ask.instrument_id,
  cursor: ask.cursor,
  limit: ask.limit === undefined ? undefined : String(ask.limit),
})

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

export async function get<T>(path: string, query: Record<string, string | undefined> = {}): Promise<T> {
  const q = new URLSearchParams()
  for (const [k, v] of Object.entries(query)) if (v !== undefined) q.set(k, v)
  const s = q.toString()
  const r = await fetch(`${k_base}${path}${s === '' ? '' : `?${s}`}`, { headers: { accept: 'application/json' } })
  if (!r.ok) throw new ApiError(r.status, (await r.json().catch(() => null)) as Problem | null)
  return (await r.json()) as T
}

async function send<T>(method: 'PUT' | 'POST' | 'DELETE', path: string, body?: unknown): Promise<T> {
  const r = await fetch(`${k_base}${path}`, {
    method,
    headers: { accept: 'application/json', ...(body === undefined ? {} : { 'content-type': 'application/json' }) },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
  })
  if (!r.ok) throw new ApiError(r.status, (await r.json().catch(() => null)) as Problem | null)
  return (r.status === 204 ? undefined : await r.json().catch(() => undefined)) as T
}

export const put = <T>(path: string, body: unknown) => send<T>('PUT', path, body)
export const post = <T>(path: string, body?: unknown) => send<T>('POST', path, body)
export const del = <T>(path: string) => send<T>('DELETE', path)

const leveragePath = (account: string) => `/v1/accounts/${encodeURIComponent(account)}/leverage`

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
  accounts: () => get<Account[]>('/v1/accounts'),
  reference: () => get<Schema<'ReferenceList'>>('/v1/reference'),
  markets: () => get<Market[]>('/v1/markets'),
  candles: (id: string, interval: string, limit = 1000) =>
    get<CandlePage>('/v1/candles', { instrument_id: id, interval, limit: String(limit) }),
  metricsHistory: (id: string, interval: string, limit = 500) =>
    get<MetricPage>('/v1/metrics/history', { instrument_id: id, interval, limit: String(limit) }),
  instruments,
  positions: () => get<Position[]>('/v1/positions'),
  orders: () => get<Orders>('/v1/orders', { open: 'true' }),
  /** orders that can no longer fill, newest first */
  closedOrders: (ask: PageAsk = {}) => get<Orders>('/v1/orders', { open: 'false', ...pageOf(ask) }),
  fills: (ask: PageAsk = {}) => get<Fills>('/v1/fills', pageOf(ask)),
  /** the ledger's increments of one kind, newest first; the route takes no market */
  pnl: (kind: PnlKind, ask: Omit<PageAsk, 'instrument_id'> = {}) => get<Pnl>('/v1/pnl', { kind, ...pageOf(ask) }),
  parents: () => get<Parents>('/v1/joe/parents'),
  streamToken: () => get<Schema<'StreamToken'>>('/v1/stream/token'),
  /** what the venue holds for one market; a private call through the account's gateway, so not for polling */
  leverage: (account: string, instrumentId: string) =>
    get<LeverageState>(leveragePath(account), { instrument_id: instrumentId }),
  /** answers the state after the change; a venue refusal is an ApiError 409 whose detail is the venue's reason */
  setLeverage: (account: string, body: SetLeverage) => put<LeverageState>(leveragePath(account), body),
}
