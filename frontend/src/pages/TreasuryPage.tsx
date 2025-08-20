import { useState } from 'react'
import { ChartFrame, colorsFor, Donut, LinePlot } from '@/components/chart'
import { BalanceTree } from '@/components/BalanceTree'
import { PanelHeader } from '@/components/shared/PanelHeader'
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

  // A venue with a spot wallet and a perp wallet is ONE slice. Two segments
  // labelled `kucoin` is a chart asking the reader to add them up.
  const byVenue = new Map<string, number>()
  for (const b of denominated) byVenue.set(b.venue, (byVenue.get(b.venue) ?? 0) + b.total)
  const ranked = [...byVenue.entries()]
    .map(([venue, total]) => ({ venue, total }))
    .sort((a, b) => b.total - a.total)
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
              <div className="num text-xl font-semibold tracking-[-0.015em] text-fg">
                {usd(total)}
              </div>
              <div className={cn('num text-xs font-medium', change >= 0 ? 'text-up' : 'text-down')}>
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
                    <div className="num text-base font-semibold text-fg">{usd(total)}</div>
                    <div className="text-2xs text-fg-dim">{k_denomination}</div>
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
          right={<span className="meta num">{reported.length} rows</span>}
        />
        <div className="min-h-0 flex-1 overflow-auto">
        <BalanceTree />
        </div>
      </section>
    </div>
  )
}
