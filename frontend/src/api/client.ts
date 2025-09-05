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

/** A refusal the server explained. Carries the RFC 9457 body, so a screen can
 *  say what went wrong instead of "something went wrong". */
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
    // The server answers problems as JSON; anything else is a proxy or a
    // process that is not admind, and pretending otherwise hides which.
    const problem = await res.json().catch(() => null)
    throw new ApiError(res.status, problem as Problem | null)
  }
  return (await res.json()) as T
}

/** A write, which needs the one header a read does not.
 *
 *  IDEMPOTENCY-KEY IS MANDATORY and the server mints the order id, not us: a
 *  retry after a timeout must reach the same order rather than a second one.
 *  The key is generated per ATTEMPT by the caller and reused across retries of
 *  that attempt — generating a fresh one on retry is the bug this guards. */
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

/** Hands an order to the desk. A 202 means QUEUED, not filled and not even
 *  accepted by the venue: the risk gate sits past this call, and its refusal
 *  arrives later on the order's own events. A screen that reads 202 as success
 *  is claiming something this answer does not say. */
export const placeOrder = (order: NewOrder, idempotencyKey: string) =>
  post<OrderAccepted>('/v1/orders', order, idempotencyKey)

type InstrumentQuery = NonNullable<paths['/v1/instruments']['get']['parameters']['query']>

export const api = {
  health: () => get<Health>('/v1/health'),
  accounts: () => get<Account[]>('/v1/accounts'),
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
  // Everything from here reads Postgres and answers 503 without a DSN. That
  // refusal is a real answer and must reach the screen — a screen that renders
  // an empty table instead is claiming nothing happened.
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

/** Every increment since a cutoff, oldest first.
 *
 *  The ledger is append-only increments and never a running total — a total is
 *  a question asked of it. So a chart of the balance over time has to walk the
 *  pages and sum them here. */
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

/** Walks every page. The catalogue is thousands of rows on an options venue
 *  and the table virtualises anyway, so the alternative — a "load more"
 *  button on a chain — would be a control that exists to serve pagination
 *  rather than the person reading the chain. */
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
