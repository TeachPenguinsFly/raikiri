import type { components } from './types'

type Schema<K extends keyof components['schemas']> = components['schemas'][K]

export type Account = Schema<'Account'>
export type AccountBalances = Schema<'AccountBalances'>
export type Listing = Schema<'Listing'>
export type Market = Schema<'Market'>
export type PnlIncrement = Schema<'PnlIncrement'>
export type PnlKind = Schema<'PnlKind'>
export type Position = Schema<'Position'>
export type Order = Schema<'Order'>
export type Fill = Schema<'Fill'>
export type Problem = Schema<'Problem'>
export type UpstreamColumn = Schema<'UpstreamColumn'>
export type NewOrder = Schema<'NewOrder'>
export type NewParentOrder = Schema<'NewParentOrder'>
export type ParentOrder = Schema<'ParentOrder'>
export type ChildOrder = Schema<'ChildOrder'>

type Page<T> = { items: T[]; next_cursor?: string | null }

const k_pnl_page = '1000'
const k_pnl_cap = 20_000

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

async function request<T>(
  url: string,
  init: { method?: string; headers?: Record<string, string>; body?: string } = {},
): Promise<T> {
  const res = await fetch(url, { ...init, headers: { Accept: 'application/json', ...init.headers } })
  if (!res.ok) {
    const problem = (await res.json().catch(() => null)) as Problem | null
    throw new ApiError(res.status, problem)
  }
  return (await res.json()) as T
}

function get<T>(path: string, params: Record<string, string | undefined> = {}): Promise<T> {
  const query = new URLSearchParams()
  for (const [key, value] of Object.entries(params)) {
    if (value !== undefined && value !== '') query.set(key, value)
  }
  return request<T>(query.size > 0 ? `${path}?${query}` : path)
}

/** 202 means queued; a risk refusal arrives later on the order's events */
export const placeOrder = (order: NewOrder, idempotencyKey: string) =>
  request<Schema<'OrderAccepted'>>('/v1/orders', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Idempotency-Key': idempotencyKey },
    body: JSON.stringify(order),
  })

const post = <T>(path: string, body?: unknown) =>
  request<T>(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
  })

/** joe runs inside strategyd; admind forwards /v1/joe/ there */
export const joe = {
  parents: () => get<{ items: ParentOrder[] }>('/v1/joe/parents'),
  parent: (id: string) => get<ParentOrder>(`/v1/joe/parents/${id}`),
  start: (order: NewParentOrder) => post<{ id: string }>('/v1/joe/parents', order),
  act: (id: string, action: 'cancel' | 'pause' | 'resume') => post<ParentOrder>(`/v1/joe/parents/${id}/${action}`),
}

export const api = {
  health: () => get<Schema<'Health'>>('/v1/health'),
  /** checked once, at the upgrade, so each connect takes a fresh one */
  streamToken: () => get<Schema<'StreamToken'>>('/v1/stream/token'),
  accounts: () => get<Account[]>('/v1/accounts'),
  upstreamIndex: (name: string) => get<Schema<'UpstreamIndex'>>(`/v1/x/${name}/`),
  upstreamTable: (name: string, path: string, params: Record<string, string | undefined> = {}) =>
    get<Schema<'UpstreamTable'>>(`/v1/x/${name}/${path}`, params),
  markets: (q?: string) => get<Market[]>('/v1/markets', { q }),
  // the routes below read postgres and answer 503 without a dsn
  openOrders: () => get<Page<Order>>('/v1/orders', { open: 'true' }),
  fills: (limit: string) => get<Page<Fill>>('/v1/fills', { limit }),
  balances: () => get<AccountBalances[]>('/v1/balances'),
  positions: () => get<Position[]>('/v1/positions'),
  pnl: (since: string, cursor: string | undefined) =>
    get<Page<PnlIncrement>>('/v1/pnl', { since, cursor, limit: k_pnl_page }),
}

/** every pnl increment since a cutoff, oldest first */
export async function allPnl(sinceNanos: string): Promise<PnlIncrement[]> {
  const out: PnlIncrement[] = []
  let cursor: string | undefined
  do {
    const page = await api.pnl(sinceNanos, cursor)
    out.push(...page.items)
    cursor = page.next_cursor || undefined
  } while (cursor !== undefined && out.length < k_pnl_cap)
  return out.sort((a, b) => (BigInt(a.ts_event) < BigInt(b.ts_event) ? -1 : 1))
}
