import { api } from '@/api/client'
import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Venue } from '@/components/shared/Venue'
import { nanosToMillis } from '@/lib/format'
import { useQuery } from '@/lib/query'

/** lists attached gateways; no endpoint checks credentials */
const k_refresh_ms = 5_000

function since(nanos: string | undefined, now: number): string {
  if (nanos === undefined) return '—'
  const seconds = Math.max(0, Math.round((now - nanosToMillis(nanos)) / 1000))
  if (seconds < 60) return `${seconds}s ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`
  return `${Math.floor(seconds / 3600)}h ago`
}

export function SettingsPage() {
  const accounts = useQuery(() => api.accounts(), [], k_refresh_ms)
  const rows = accounts.state === 'ready' ? accounts.data : []
  const alive = rows.filter((a) => a.gateway.alive).length
  const now = Date.now()

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Attached gateways"
          right={
            accounts.state === 'ready' ? (
              <span className="meta">
                {alive}/{rows.length} answering
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
                  <th className="col-head px-3 text-left">Venue</th>
                  <th className="col-head px-3 text-left">Account</th>
                  <th className="col-head w-24 px-3 text-left">Market</th>
                  <th className="col-head w-32 px-3 text-left">Gateway</th>
                  <th className="col-head w-28 px-3 text-right">Instruments</th>
                  <th className="col-head w-28 px-3 text-right">Heartbeat</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((a) => (
                  <tr key={a.account} className="row">
                    <td className="px-3">
                      <Venue venue={a.venue} />
                    </td>
                    <td className="px-3 text-fg">{a.account}</td>
                    <td className="px-3 text-fg-mid">{a.market}</td>
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
                    <td className="num px-3 text-right text-xs text-fg-dim">
                      {since(a.gateway.last_heartbeat, now)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      </section>
    </div>
  )
}
