import type { components, paths } from './types'

export type Schema<K extends keyof components['schemas']> = components['schemas'][K]

export type Account = Schema<'Account'>
export type AccountBalances = Schema<'AccountBalances'>
export type Balance = Schema<'Balance'>
export type Health = Schema<'Health'>
export type Instrument = Schema<'Instrument'>
export type Listing = Schema<'Listing'>
export type Market = Schema<'Market'>
export type PnlIncrement = Schema<'PnlIncrement'>
export type PnlKind = Schema<'PnlKind'>
export type Position = Schema<'Position'>
export type Order = Schema<'Order'>
export type Fill = Schema<'Fill'>
export type Problem = Schema<'Problem'>
export type UpstreamTable = Schema<'UpstreamTable'>
export type UpstreamColumn = Schema<'UpstreamColumn'>
export type UpstreamIndex = Schema<'UpstreamIndex'>

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

async function get<T>(path: string, params?: Record<string, string | undefined>): Promise<T> {
  const query = new URLSearchParams()
  for (const [key, value] of Object.entries(params ?? {})) {
    if (value !== undefined && value !== '') query.set(key, value)
  }
  const url = query.size > 0 ? `${path}?${query}` : path
  const res = await fetch(url, { headers: { Accept: 'application/json' } })
  if (!res.ok) {
    const problem = await res.json().catch(() => null)
    throw new ApiError(res.status, problem as Problem | null)
  }
  return (await res.json()) as T
}

/** the caller reuses one idempotency key across retries of the same attempt */
async function post<T>(path: string, body: unknown, idempotencyKey: string): Promise<T> {
  const res = await fetch(path, {
    method: 'POST',
    headers: {
      Accept: 'application/json',
      'Content-Type': 'application/json',
      'Idempotency-Key': idempotencyKey,
    },
    body: JSON.stringify(body),
  })
  if (!res.ok) {
    const problem = await res.json().catch(() => null)
    throw new ApiError(res.status, problem as Problem | null)
  }
  return (await res.json()) as T
}

export type NewOrder = Schema<'NewOrder'>
export type OrderAccepted = Schema<'OrderAccepted'>

/** 202 means queued; a risk refusal arrives later on the order's events */
export const placeOrder = (order: NewOrder, idempotencyKey: string) =>
  post<OrderAccepted>('/v1/orders', order, idempotencyKey)

type InstrumentQuery = NonNullable<paths['/v1/instruments']['get']['parameters']['query']>

export const api = {
  health: () => get<Health>('/v1/health'),
  accounts: () => get<Account[]>('/v1/accounts'),
  upstreamIndex: (name: string) => get<UpstreamIndex>(`/v1/x/${name}/`),
  upstreamTable: (name: string, path: string) => get<UpstreamTable>(`/v1/x/${name}/${path}`),
  instrument: (id: string) => get<Instrument>(`/v1/instruments/${id}`),
  markets: (params: { q?: string | undefined; limit?: string | undefined } = {}) =>
    get<Market[]>('/v1/markets', { q: params.q, limit: params.limit }),
  instruments: (params: {
    account?: string | undefined
    kind?: InstrumentQuery['kind'] | undefined
    cursor?: string | undefined
    limit?: string | undefined
  }) =>
    get<{ items: Instrument[]; next_cursor?: string | null }>('/v1/instruments', {
      account: params.account,
      kind: params.kind,
      cursor: params.cursor,
      limit: params.limit,
    }),
  // the routes below read postgres and answer 503 without a dsn
  orders: (params: { account?: string | undefined; open?: boolean | undefined } = {}) =>
    get<{ items: Order[]; next_cursor?: string | null }>('/v1/orders', {
      account: params.account,
      open: params.open === undefined ? undefined : String(params.open),
    }),
  fills: (params: { account?: string | undefined; limit?: string | undefined } = {}) =>
    get<{ items: Fill[]; next_cursor?: string | null }>('/v1/fills', {
      account: params.account,
      limit: params.limit,
    }),
  balances: (params: { account?: string | undefined } = {}) =>
    get<AccountBalances[]>('/v1/balances', { account: params.account }),
  positions: (params: { account?: string | undefined } = {}) =>
    get<Position[]>('/v1/positions', { account: params.account }),
  pnl: (params: {
    account?: string | undefined
    kind?: PnlKind | undefined
    since?: string | undefined
    cursor?: string | undefined
    limit?: string | undefined
  }) =>
    get<{ items: PnlIncrement[]; next_cursor?: string | null }>('/v1/pnl', {
      account: params.account,
      kind: params.kind,
      since: params.since,
      cursor: params.cursor,
      limit: params.limit,
    }),
}

/** every pnl increment since a cutoff, oldest first */
export async function allPnl(sinceNanos?: string): Promise<PnlIncrement[]> {
  const out: PnlIncrement[] = []
  let cursor: string | undefined
  do {
    const page = await api.pnl({ since: sinceNanos, cursor, limit: '1000' })
    out.push(...page.items)
    cursor = page.next_cursor ?? undefined
  } while (cursor && out.length < 20_000)
  return out.sort((a, b) => (BigInt(a.ts_event) < BigInt(b.ts_event) ? -1 : 1))
}

export async function allInstruments(
  params: { account?: string | undefined; kind?: InstrumentQuery['kind'] | undefined },
  onPage?: (total: number) => void,
): Promise<Instrument[]> {
  const out: Instrument[] = []
  let cursor: string | undefined
  do {
    const page = await api.instruments({ ...params, cursor, limit: '1000' })
    out.push(...page.items)
    onPage?.(out.length)
    cursor = page.next_cursor ?? undefined
  } while (cursor)
  return out
}
