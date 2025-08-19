import { useState } from 'react'
import { Link } from 'react-router'
import { ChartFrame, colorsFor, LinePlot } from '@/components/chart'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { HOURS, STRATEGIES, STRATEGY_PNL } from '@/mock/data'
import { decompose, STRATEGY_DETAIL } from '@/mock/strategies'

const usd = (v: number) =>
  v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })

/** Traded notional, taken from the strategy's own fills rather than stored
 *  twice. A figure kept in two places is a figure that will disagree. */
const volume = (name: string) => {
  const detail = STRATEGY_DETAIL[name]
  if (detail === undefined) return '—'
  return `${decompose(detail).volumeUsd.toLocaleString('en-US', { maximumFractionDigits: 0 })}`
}

export function StrategiesPage() {
  const [hover, setHover] = useState<string | null>(null)
  const entering = useEntranceOnce(true, 'strategies')

  const total = STRATEGIES.reduce((sum, s) => sum + s.pnl, 0)
  const colors = colorsFor(STRATEGIES.map((s) => s.name))

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2 pt-0">
      <ChartFrame
        className="shrink-0"
        title="PnL by strategy"
        readout={
          <div className="text-right">
            <div className={cn('num text-xl font-semibold tracking-[-0.015em]', total >= 0 ? 'text-up' : 'text-down')}>
              {total >= 0 ? '+' : '−'}
              {usd(Math.abs(total))}
            </div>
            <div className="text-xs text-fg-dim">USDT</div>
          </div>
        }
        height={250}
        legend={STRATEGIES.map((s) => ({
          key: s.name,
          label: s.name,
          color: colors[s.name] as string,
          value: `${s.pnl >= 0 ? '+' : '−'}${usd(Math.abs(s.pnl))}`,
          muted: s.status !== 'alive',
        }))}
        onLegendHover={setHover}
      >
        {({ width, height }) => (
          <LinePlot
            x={HOURS}
            series={STRATEGIES.map((s) => ({
              key: s.name,
              label: s.name,
              color: colors[s.name] as string,
              values: STRATEGY_PNL[s.name] ?? [],
            }))}
            width={width}
            height={height}
            highlight={hover}
          />
        )}
      </ChartFrame>

      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Strategies"
          right={
            <span className="meta num">
              {STRATEGIES.filter((s) => s.status === 'alive').length} alive
            </span>
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full border-collapse text-sm whitespace-nowrap">
          <thead className="sticky top-0 z-10 bg-panel">
            <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
              <th className="col-head px-3 text-left">Name</th>
              <th className="col-head px-3 text-left">Kind</th>
              <th className="col-head px-3 text-left">Status</th>
              <th className="col-head px-3 text-right">Orders</th>
              <th className="col-head px-3 text-right">Trades</th>
              <th className="col-head px-3 text-right">Volume</th>
              <th className="col-head px-3 text-right">PnL</th>
            </tr>
          </thead>
          <tbody>
            {STRATEGIES.map((s, i) => (
              <tr
                key={s.name}
                style={entering ? enterDelay(i) : undefined}
                onMouseEnter={() => setHover(s.name)}
                onMouseLeave={() => setHover(null)}
                className={cn(
                  'row',
                  entering && 'enter-fade-up',
                  hover === s.name && 'bg-hover',
                )}
              >
                <td className="px-3">
                  <Link
                    to={`/strategies/${s.name}`}
                    className="inline-flex items-center gap-2 hover:text-accent"
                  >
                    <span
                      className="size-1.5 shrink-0 rounded-full"
                      style={{ background: colors[s.name] as string }}
                    />
                    <span className="font-medium text-fg">{s.name}</span>
                  </Link>
                </td>
                <td className="px-3 text-fg-mid">{s.kind}</td>
                <td className="px-3">
                  <StatusBadge status={s.status} />
                </td>
                {/* A strategy alive with no orders is a strategy that is not
                    trading, and the zero has to be readable as that. */}
                <td className={cn('num px-3 text-right', s.orders === 0 ? 'text-fg-dim' : 'text-fg')}>
                  {s.orders}
                </td>
                {/* Fills, not orders. A maker strategy with nothing working can
                    still be the one that has traded the most today. */}
                <td className={cn('num px-3 text-right', s.trades === 0 ? 'text-fg-dim' : 'text-fg')}>
                  {s.trades}
                </td>
                {/* Traded notional. Beside the fill count it says whether the
                    strategy is moving size or clipping — two strategies with
                    the same number of fills can be doing different jobs. */}
                <td className="num px-3 text-right text-fg-mid">{volume(s.name)}</td>
                <td className={cn('num px-3 text-right', s.pnl >= 0 ? 'text-up' : 'text-down')}>
                  {s.pnl >= 0 ? '+' : '−'}
                  {usd(Math.abs(s.pnl))}
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
