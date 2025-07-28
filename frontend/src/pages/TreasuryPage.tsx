import { useState } from 'react'
import { ChartFrame, colorsFor, Donut, LinePlot } from '@/components/chart'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { VenueTag } from '@/components/shared/VenueTag'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { BALANCES, EQUITY, HOURS } from '@/mock/data'

/** The asset the desk is denominated in. Only this one is summed across
 *  venues: adding USDT to XRP produces a figure nobody holds. */
const k_denomination = 'USDT'

const usd = (v: number) =>
  v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })

export function TreasuryPage() {
  const [hover, setHover] = useState<string | null>(null)

  const reported = BALANCES.filter((b) => b.reported)
  const denominated = reported.filter((b) => b.asset === k_denomination)
  const total = denominated.reduce((sum, b) => sum + b.total, 0)
  const entering = useEntranceOnce(true, 'treasury')

  const ranked = [...denominated].sort((a, b) => b.total - a.total)
  const colors = colorsFor(ranked.map((b) => b.venue))
  const segments = ranked.map((b) => ({
    key: b.venue,
    label: b.venue,
    color: colors[b.venue] as string,
    value: b.total,
  }))

  const change = ((EQUITY.at(-1) as number) - (EQUITY[0] as number)) / (EQUITY[0] as number)

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2 pt-0">
      <div className="grid shrink-0 grid-cols-1 items-start gap-2 xl:grid-cols-[minmax(0,1fr)_380px]">
        <ChartFrame
          title="Equity"
          readout={
            <div className="text-right">
              <div className="num text-[19px] font-bold leading-6 text-fg">{usd(total)}</div>
              <div className={cn('num text-2xs font-semibold', change >= 0 ? 'text-up' : 'text-down')}>
                {change >= 0 ? '+' : '−'}
                {(Math.abs(change) * 100).toFixed(2)}% · 96h
              </div>
            </div>
          }
          height={260}
        >
          {({ width, height }) => (
            <LinePlot
              x={HOURS}
              series={[
                { key: 'equity', label: 'Equity', color: 'var(--color-accent)', values: EQUITY, area: true },
              ]}
              width={width}
              height={height}
              baseline="data"
            />
          )}
        </ChartFrame>

        <ChartFrame
          title="Split by venue"
          height={228}
          legend={segments.map((s) => ({
            key: s.key,
            label: s.label,
            color: s.color,
            value: `${((s.value / total) * 100).toFixed(1)}%`,
          }))}
          onLegendHover={setHover}
        >
          {({ height }) => (
            <div className="flex h-full items-center justify-center">
              <Donut
                segments={segments}
                size={Math.min(height, 200)}
                active={hover}
                onHover={setHover}
                center={
                  <div className="text-center">
                    <div className="num text-[15px] font-bold text-fg">{usd(total)}</div>
                    <div className="text-3xs text-fg-dim">{k_denomination}</div>
                  </div>
                }
              />
            </div>
          )}
        </ChartFrame>
      </div>

      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Balances"
          right={<span className="micro-label num">{reported.length} rows</span>}
        />
        <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full border-collapse text-[12.5px]">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr className="border-b border-line text-3xs uppercase tracking-[0.06em] text-fg-mid">
              <th className="h-7 px-2.5 text-left font-medium">Account</th>
              <th className="h-7 px-2 text-left font-medium">Market</th>
              <th className="h-7 px-2 text-left font-medium">Asset</th>
              <th className="h-7 px-2 text-right font-medium">Total</th>
              <th className="h-7 px-2 text-right font-medium">Available</th>
              <th className="h-7 px-2.5 text-right font-medium">Share</th>
            </tr>
          </thead>
          <tbody>
            {reported.map((b, i) => (
              <tr
                key={`${b.account}:${b.asset}`}
                style={entering ? enterDelay(i) : undefined}
                onMouseEnter={() => setHover(b.venue)}
                onMouseLeave={() => setHover(null)}
                className={cn(
                  'row-hover border-b border-line/40',
                  entering && 'enter-fade-up',
                  hover === b.venue && 'bg-hover',
                )}
              >
                <td className="px-2.5 py-1.5">
                  <VenueTag venue={b.venue} full />
                </td>
                {/* The market is read off the account name — perp is the
                    unsuffixed default, as format_account writes it. */}
                <td className="px-2 py-1.5 text-3xs text-fg-mid">
                  {b.account === b.venue ? 'perp' : b.account.slice(b.venue.length + 1)}
                </td>
                <td className="px-2 py-1.5 font-semibold text-fg">{b.asset}</td>
                <td className="num px-2 py-1.5 text-right">{usd(b.total)}</td>
                <td className="num px-2 py-1.5 text-right text-fg">
                  {b.available === null ? (
                    <span
                      className="text-fg-dim"
                      title="this venue publishes no per-asset free figure"
                    >
                      —
                    </span>
                  ) : (
                    usd(b.available)
                  )}
                </td>
                {/* Only the denominated rows have a share. A share of a total
                    that does not include you is not a share. */}
                <td className="num px-2.5 py-1.5 text-right text-fg-mid">
                  {b.asset === k_denomination ? `${((b.total / total) * 100).toFixed(1)}%` : '—'}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        </div>
      </section>
    </div>
  )
}
