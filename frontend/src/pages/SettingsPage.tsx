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
            <span className="micro-label num">
              {gateways.filter((g) => g.alive).length}/{gateways.length} attached
            </span>
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full border-collapse text-[11.5px]">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr className="border-b border-line text-3xs uppercase tracking-[0.08em] text-fg-dim">
              <th className="h-7 px-2.5 text-left font-medium">Account</th>
              <th className="h-7 px-2 text-left font-medium">Market</th>
              <th className="h-7 px-2 text-left font-medium">Segment</th>
              <th className="h-7 px-2 text-right font-medium">Instruments</th>
              <th className="h-7 px-2 text-left font-medium">Credential</th>
              <th className="h-7 px-2.5 text-left font-medium">State</th>
            </tr>
          </thead>
          <tbody>
            {gateways.map((g, i) => (
              <tr
                key={g.account}
                style={entering ? enterDelay(i) : undefined}
                className={cn('row-hover border-b border-line/40', entering && 'enter-fade-up')}
              >
                <td className="px-2.5 py-1.5">
                  <VenueTag venue={g.venue} full />
                </td>
                <td className="px-2 py-1.5 text-fg-dim">{g.market}</td>
                {/* One segment per (venue, market), and perp is the unsuffixed
                    default — so the path a reader sees here is the path on disk. */}
                <td className="px-2 py-1.5 font-mono text-3xs text-fg-mid">gw.{g.account}</td>
                <td className={cn('num px-2 py-1.5 text-right', g.alive ? 'text-fg-mid' : 'text-fg-dim')}>
                  {g.alive ? g.instruments : '—'}
                </td>
                <td className="px-2 py-1.5 font-mono text-3xs text-fg-dim">
                  {SOURCES[g.venue] ?? (
                    <span className="text-amber">not configured</span>
                  )}
                </td>
                <td className="px-2.5 py-1.5">
                  <StatusBadge status={g.alive ? 'alive' : 'dead'} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        </div>
        <div className="shrink-0 border-t border-line px-2.5 py-2 text-3xs leading-relaxed text-fg-dim">
          Credentials are passed to a gateway as a reference — <span className="font-mono">env:</span>,{' '}
          <span className="font-mono">file:</span> or <span className="font-mono">exec:</span> — and
          resolved inside the process. No key is written to a config file or a journal, and none is
          ever sent to this screen.
        </div>
      </section>

      <div className="grid shrink-0 grid-cols-1 gap-2 lg:grid-cols-2">
        <section className="panel flex flex-col">
          <PanelHeader title="Transport" />
          <dl className="divide-y divide-line/60 text-[11.5px]">
            <Row term="Live data" value="WebSocket, one stream per gateway" />
            <Row term="Book cadence" value="1 Hz, coalesced" note="state is sampled; events are sequenced" />
            <Row term="Depth" value="25 levels" />
            <Row term="Catalogue" value="polled on attach, then on generation change" />
          </dl>
        </section>

        <section className="panel flex flex-col">
          <PanelHeader title="Shared memory" />
          <dl className="divide-y divide-line/60 text-[11.5px]">
            <Row term="ABI version" value="4" />
            <Row term="Channels" value="11 of 12" />
            <Row term="Readers" value="4 slots" note="a fifth attach is refused, not queued" />
            <Row term="Segment root" value="/dev/shm" />
          </dl>
        </section>
      </div>

      <section className="panel shrink-0 px-3 py-2.5 text-3xs leading-relaxed text-fg-dim">
        Every figure on this build is a fixture. The backend serves reads only — order entry needs a
        producer into strategyd, and nothing on this screen writes.
      </section>
    </div>
  )
}

function Row({ term, value, note }: { term: string; value: string; note?: string }) {
  return (
    <div className="flex items-baseline justify-between gap-4 px-2.5 py-2">
      <dt className="shrink-0 text-fg-dim">{term}</dt>
      <dd className="text-right">
        <span className="text-fg-mid">{value}</span>
        {note !== undefined && <div className="text-3xs text-fg-dim">{note}</div>}
      </dd>
    </div>
  )
}
