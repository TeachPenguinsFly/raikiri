import { useMemo, useRef } from 'react'
import { cn, EmptyState, nanosToMillis, PanelHeader, Venue } from '@raikiri/ui'
import { api, type Account } from '../api/client'
import { useQuery } from '../lib/query'

const k_refresh_ms = 5_000

const k_market_order = ['perp', 'spot', 'future', 'option']

function since(millis: number | undefined, now: number): string {
  if (millis === undefined) return '—'
  const seconds = Math.max(0, (now - millis) / 1000)
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

export function SettingsPage() {
  const accounts = useQuery(() => api.accounts(), [], k_refresh_ms)
  const rows = useMemo(() => (accounts.state === 'ready' ? accounts.data : []), [accounts])
  const heartbeats = useHeartbeats(rows)
  const alive = rows.filter((a) => a.gateway.alive).length
  const now = Date.now()

  const venues = useMemo(() => {
    const out = new Map<string, Account[]>()
    for (const a of rows) out.set(a.venue, [...(out.get(a.venue) ?? []), a])
    for (const list of out.values()) {
      list.sort((a, b) => marketRank(a.market) - marketRank(b.market) || a.account.localeCompare(b.account))
    }
    return [...out].sort(([a], [b]) => a.localeCompare(b))
  }, [rows])

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Venues"
          right={
            accounts.state === 'ready' ? (
              <span className="meta">
                {venues.length} venues · {alive}/{rows.length} gateways answering
              </span>
            ) : null
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
          {accounts.state === 'loading' && <EmptyState title="Asking admind" />}

          {accounts.state === 'failed' && (
            <div className="px-3 py-6 text-xs">
              <div className="text-down">
                {accounts.status !== null && (
                  <span className="num mr-2 text-fg-dim">{accounts.status}</span>
                )}
                {accounts.title}
              </div>
              {accounts.detail !== undefined && (
                <div className="mt-1 text-fg-dim">{accounts.detail}</div>
              )}
            </div>
          )}

          {accounts.state === 'ready' && rows.length === 0 && (
            <EmptyState
              title="No gateway is attached"
              hint="a venue whose key is missing or refused never attaches one"
            />
          )}

          {accounts.state === 'ready' && rows.length > 0 && (
            <table className="w-full border-collapse text-sm whitespace-nowrap">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                  <th className="col-head w-48 px-3 text-left">Venue</th>
                  <th className="col-head w-24 px-3 text-left">Market</th>
                  <th className="col-head px-3 text-left">Account</th>
                  <th className="col-head w-32 px-3 text-left">Gateway</th>
                  <th className="col-head w-28 px-3 text-right">Instruments</th>
                  <th className="col-head w-24 px-3 text-right">Priced</th>
                  <th className="col-head w-28 px-3 text-right">Heartbeat</th>
                </tr>
              </thead>
              {venues.map(([venue, list]) => (
                <tbody key={venue} className="border-t border-line/60 first-of-type:border-t-0">
                  {list.map((a, i) => (
                    <tr key={a.account} className="row">
                      {i === 0 && (
                        <td rowSpan={list.length} className="px-3 align-top">
                          <div className="flex h-7 items-center gap-2">
                            <Venue venue={venue} />
                            {list.length > 1 && (
                              <span className="num text-2xs text-fg-dim">{list.length}</span>
                            )}
                          </div>
                        </td>
                      )}
                      <td className="px-3 text-fg capitalize">{a.market}</td>
                      <td className="px-3 text-xs text-fg-dim">{a.account}</td>
                      <td className="px-3">
                        {a.gateway.alive ? (
                          <span className="text-up">alive</span>
                        ) : (
                          <span className="text-down">not answering</span>
                        )}
                      </td>
                      <td className="num px-3 text-right text-fg-mid">
                        {a.gateway.alive ? a.gateway.instruments.toLocaleString() : '—'}
                      </td>
                      <td
                        className={cn(
                          'num px-3 text-right',
                          a.gateway.coverage_pct < 50 ? 'text-amber' : 'text-fg-mid',
                        )}
                        title={`${a.gateway.priced.toLocaleString()} of ${a.gateway.instruments.toLocaleString()} have quoted`}
                      >
                        {a.gateway.alive ? `${a.gateway.coverage_pct}%` : '—'}
                      </td>
                      <td className="num px-3 text-right text-xs text-fg-dim">
                        {since(heartbeats.get(a.account), now)}
                      </td>
                    </tr>
                  ))}
                </tbody>
              ))}
            </table>
          )}
        </div>
      </section>
    </div>
  )
}
