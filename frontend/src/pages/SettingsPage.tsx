import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { VenueTag } from '@/components/shared/VenueTag'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { useGateways } from '@/mock/data'

/** How a credential reaches a gateway. A REFERENCE, never a value.
 *
 *  This screen shows the reference and nothing else, because the value is not
 *  something the terminal is ever given: a secret that reaches the browser has
 *  been in a process, a log and a devtools panel on the way. */
const SOURCES: Record<string, string> = {
  bybit: 'env:RAIKIRI_BYBIT_KEY',
  binance: 'env:RAIKIRI_BINANCE_KEY',
  okx: 'env:RAIKIRI_OKX_KEY',
  gate: 'env:RAIKIRI_GATE_KEY',
  kucoin: 'env:RAIKIRI_KUCOIN_KEY',
  xt: 'env:RAIKIRI_XT_KEY',
}

export function SettingsPage() {
  const gateways = useGateways()
  const entering = useEntranceOnce(true, 'settings')

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2 pt-0">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Gateways"
          right={
            <span className="meta num">
              {gateways.filter((g) => g.alive).length}/{gateways.length} attached
            </span>
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full border-collapse text-sm whitespace-nowrap">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
              <th className="col-head px-3 text-left font-medium">Account</th>
              <th className="col-head px-3 text-left font-medium">Market</th>
              <th className="col-head px-3 text-left font-medium">Segment</th>
              <th className="col-head px-3 text-right font-medium">Instruments</th>
              <th className="col-head px-3 text-left font-medium">Credential</th>
              <th className="col-head px-3 text-left font-medium">State</th>
            </tr>
          </thead>
          <tbody>
            {gateways.map((g, i) => (
              <tr
                key={g.account}
                style={entering ? enterDelay(i) : undefined}
                className={cn('row', entering && 'enter-fade-up')}
              >
                <td className="px-3">
                  <VenueTag venue={g.venue} full />
                </td>
                <td className="px-3 text-fg-mid">{g.market}</td>
                {/* One segment per (venue, market), and perp is the unsuffixed
                    default — so the path a reader sees here is the path on disk. */}
                <td className="px-3 font-mono text-2xs text-fg">gw.{g.account}</td>
                <td className={cn('num px-3 text-right', g.alive ? 'text-fg-mid' : 'text-fg-dim')}>
                  {g.alive ? g.instruments : '—'}
                </td>
                <td className="px-3 font-mono text-2xs text-fg-mid">
                  {SOURCES[g.venue] ?? (
                    <span className="text-amber">not configured</span>
                  )}
                </td>
                <td className="px-3">
                  <StatusBadge status={g.alive ? 'alive' : 'dead'} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {/* Said once, under the column it is about, rather than pinned to the
            bottom of a panel three hundred pixels below it. */}
        <p className="px-3 pt-3 text-2xs text-fg-dim">
          References, never values — no key reaches this screen.
        </p>
        </div>
      </section>
    </div>
  )
}
