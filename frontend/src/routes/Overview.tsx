import { api } from '@/api/client'
import { Failed, Loading, Panel } from '@/components/Chrome'
import { useQuery } from '@/lib/query'

/** Nanoseconds on the gateway's MONOTONIC clock, so this is an age, never a
 *  wall time. Rendering it as a date would put a 1970 timestamp on screen. */
function staleness(heartbeat: string, generation: string): string {
  const ns = BigInt(heartbeat) - BigInt(generation)
  const seconds = Number(ns / 1_000_000_000n)
  if (seconds < 60) return `${seconds}s`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m`
  return `${Math.floor(seconds / 3600)}h`
}

export function Overview() {
  const health = useQuery(() => api.health(), [], 2000)

  if (health.state === 'loading') return <Loading what="gateways" />
  if (health.state === 'failed') return <Failed {...health} />

  const { gateways, database } = health.data
  const instruments = gateways.reduce((n, g) => n + g.gateway.instruments, 0)

  return (
    <div className="h-full space-y-4 overflow-auto p-5">
      <div className="grid grid-cols-2 gap-4 md:grid-cols-4">
        <Panel title="Gateways">
          <span className="tnum text-2xl">{gateways.length}</span>
        </Panel>
        <Panel title="Answering">
          <span className="tnum text-2xl">{gateways.filter((g) => g.gateway.alive).length}</span>
        </Panel>
        <Panel title="Instruments">
          <span className="tnum text-2xl">{instruments.toLocaleString()}</span>
        </Panel>
        <Panel title="Database">
          <span className={database.reachable ? 'text-up' : 'text-warn'}>
            {database.reachable ? 'reachable' : 'not reachable'}
          </span>
        </Panel>
      </div>

      <Panel title="Attached gateways">
        {gateways.length === 0 ? (
          <p className="text-sm text-muted">
            None. admind is running and found no segments — start a gateway.
          </p>
        ) : (
          <table className="w-full text-sm">
            <thead className="text-left text-xs uppercase tracking-wider text-faint">
              <tr>
                <th className="pb-2 font-normal">Account</th>
                <th className="pb-2 font-normal">Venue</th>
                <th className="pb-2 font-normal">Market</th>
                <th className="pb-2 text-right font-normal">Instruments</th>
                <th className="pb-2 text-right font-normal">Uptime</th>
                <th className="pb-2 text-right font-normal">Owner</th>
              </tr>
            </thead>
            <tbody>
              {gateways.map((g) => (
                <tr key={g.account} className="border-t border-line/60">
                  <td className="py-1.5 font-mono">{g.account}</td>
                  <td className="py-1.5 text-muted">{g.venue}</td>
                  <td className="py-1.5 text-muted">{g.market}</td>
                  <td className="tnum py-1.5 text-right">
                    {g.gateway.instruments.toLocaleString()}
                  </td>
                  <td className="tnum py-1.5 text-right text-muted">
                    {staleness(g.gateway.last_heartbeat ?? '0', g.gateway.generation)}
                  </td>
                  <td className="py-1.5 text-right">
                    <span className={g.gateway.alive ? 'text-up' : 'text-down'}>
                      {g.gateway.alive ? 'alive' : 'not answering'}
                    </span>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Panel>
    </div>
  )
}
