import { useMemo, useRef } from 'react'
import { cn, EmptyState, ExchangeIcon, nanosToMillis, venueLabel } from '@raikiri/ui'
import { api, type Account } from '../api/client'
import { useQuery } from '../lib/query'

const k_refresh_ms = 5_000

const k_market_order = ['perp', 'spot', 'future', 'option']

function since(millis: number | undefined, now: number): string {
  if (millis === undefined) return '—'
  const seconds = Math.max(0, (now - millis) / 1000)
  if (seconds < 1) return 'just now'
  if (seconds < 10) return `${seconds.toFixed(1)}s ago`
  if (seconds < 60) return `${Math.round(seconds)}s ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`
  return `${Math.floor(seconds / 3600)}h ago`
}

/** admind answers last_heartbeat "0" now and then for a gateway that is beating, so a zero
 *  keeps the beat seen before it rather than reading as the unix epoch */
function useHeartbeats(rows: Account[]): Map<string, number> {
  const seen = useRef(new Map<string, number>())
  return useMemo(() => {
    for (const a of rows) {
      const nanos = a.gateway.last_heartbeat
      if (nanos !== undefined && nanos !== '0') seen.current.set(a.account, nanosToMillis(nanos))
    }
    return new Map(seen.current)
  }, [rows])
}

const marketRank = (market: string) => {
  const rank = k_market_order.indexOf(market)
  return rank < 0 ? k_market_order.length : rank
}

const k_market: Record<string, string> = { perp: 'Perpetual', spot: 'Spot', option: 'Options', future: 'Futures' }

/** the same words the top bar's feeds use */
function stateOf(a: Account): { word: string; tone: string } {
  if (!a.gateway.alive) return { word: 'Down', tone: 'bg-down' }
  if (a.gateway.coverage_pct < 50) return { word: 'Warming up', tone: 'bg-amber' }
  return { word: 'Live', tone: 'bg-up' }
}

export function SettingsPage() {
  const accounts = useQuery(() => api.accounts(), [], k_refresh_ms)
  const rows = useMemo(() => (accounts.state === 'ready' ? accounts.data : []), [accounts])
  const heartbeats = useHeartbeats(rows)
  const now = Date.now()

  const sorted = useMemo(
    () =>
      [...rows].sort(
        (a, b) =>
          venueLabel(a.venue).localeCompare(venueLabel(b.venue)) ||
          marketRank(a.market) - marketRank(b.market) ||
          a.account.localeCompare(b.account),
      ),
    [rows],
  )

  return (
    <section className="panel h-full min-h-0 overflow-auto">
      {accounts.state === 'loading' && <EmptyState title="Asking admind" />}
      {accounts.state === 'failed' && (
        <div className="px-4 py-6 text-sm text-down">
          {accounts.title}
          {accounts.detail !== undefined && <div className="mt-1 text-fg-dim">{accounts.detail}</div>}
        </div>
      )}
      {accounts.state === 'ready' && rows.length === 0 && (
        <EmptyState title="No feeds running" hint="A venue whose key is missing or refused never starts one." />
      )}
      {accounts.state === 'ready' && rows.length > 0 && (
        <table className="w-full border-collapse text-sm whitespace-nowrap">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr className="h-10 shadow-[inset_0_-1px_0_0_var(--color-line)]">
              <th className="col-head px-4 text-left">Feed</th>
              <th className="col-head px-4 text-left">State</th>
              <th className="col-head px-4 text-right">Markets priced</th>
              <th className="col-head px-4 text-right">Last heard</th>
            </tr>
          </thead>
          <tbody>
            {sorted.map((a, i) => {
              const state = stateOf(a)
              const first = i === 0 || sorted[i - 1]!.venue !== a.venue
              return (
                <tr key={a.account} className={cn('row h-10', first && i > 0 && 'border-t border-line/60')}>
                  <td className="px-4">
                    <span className="flex items-center gap-2.5">
                      <ExchangeIcon venue={a.venue} className={cn('size-4', !first && 'invisible')} />
                      <span className={cn('w-28', first ? 'text-fg' : 'text-transparent')}>{venueLabel(a.venue)}</span>
                      <span className="text-fg-mid">{k_market[a.market] ?? a.market}</span>
                    </span>
                  </td>
                  <td className="px-4">
                    <span className="flex items-center gap-2 text-fg-mid">
                      <span className={cn('size-2 rounded-full', state.tone)} />
                      {state.word}
                    </span>
                  </td>
                  <td className="mono px-4 text-right text-fg-mid">
                    {a.gateway.alive
                      ? `${a.gateway.priced.toLocaleString('en-US')} of ${a.gateway.instruments.toLocaleString('en-US')}`
                      : '—'}
                  </td>
                  <td className="mono px-4 text-right text-fg-dim">{since(heartbeats.get(a.account), now)}</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}
    </section>
  )
}
