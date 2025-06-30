import type { components, paths } from './types'

export type Schema<K extends keyof components['schemas']> = components['schemas'][K]

export type Account = Schema<'Account'>
export type Health = Schema<'Health'>
export type Instrument = Schema<'Instrument'>
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

type InstrumentQuery = NonNullable<paths['/v1/instruments']['get']['parameters']['query']>

export const api = {
  health: () => get<Health>('/v1/health'),
  accounts: () => get<Account[]>('/v1/accounts'),
  instrument: (id: string) => get<Instrument>(`/v1/instruments/${id}`),
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
