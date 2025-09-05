import { useState } from 'react'
import { allPnl, api, type PnlKind } from '@/api/client'
import { BalanceRows } from '@/components/BalanceTree'
import { ChartFrame, colorsFor, Donut, kindColor, LinePlot } from '@/components/chart'
import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { nanosToMillis, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'
import { cn } from '@/lib/utils'

/** The asset the desk is denominated in. Only this one is summed across
 *  venues: adding USDT to XRP produces a figure nobody holds. */
const k_denomination = 'USDT'

/** How far back the ledger is walked. A cumulative curve is cumulative SINCE
 *  something, and the window is most of what the number means. */
const k_window_hours = 96

const k_balances_refresh_ms = 5_000

/** The walk is a request per thousand increments, so it is asked for far less
 *  often than the balances are. */
const k_pnl_refresh_ms = 60_000

const usd = (v: number) =>
  v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })
const signed = (v: number) => `${v >= 0 ? '+' : '−'}${usd(Math.abs(v))}`

/** The window start as the contract's `since`: nanoseconds as a string,
 *  because a millisecond count times a million is already past 2^53. */
const windowStart = () => (BigInt(Date.now() - k_window_hours * 3_600_000) * 1_000_000n).toString()

/** A refusal, in the server's own words. `/v1/pnl` without a DSN explains
 *  itself better than any wording here would, and an empty chart in its place
 *  would say that nothing happened. */
const trouble = (title: string, detail?: string | undefined) =>
  detail === undefined ? title : `${title} — ${detail}`

export function TreasuryPage() {
  const [hover, setHover] = useState<string | null>(null)
  const [kind, setKind] = useState<string | null>(null)

  const balances = useQuery(() => api.balances(), [], k_balances_refresh_ms)
  const pnl = useQuery(() => allPnl(windowStart()), [], k_pnl_refresh_ms)

  const accounts = balances.state === 'ready' ? balances.data : []
  const reporting = accounts.filter((a) => a.reported_at !== null && a.reported_at !== undefined)

  // A venue with a spot wallet and a perp wallet is ONE slice. Two segments
  // labelled `kucoin` is a chart asking the reader to add them up. A venue that
  // published no total is absent from the split rather than a zero slice.
  const byVenue = new Map<string, number>()
  for (const account of reporting) {
    for (const b of account.assets) {
      if (b.asset !== k_denomination) continue
      if (b.total === undefined || b.total === null) continue
      byVenue.set(account.venue, (byVenue.get(account.venue) ?? 0) + toNumber(b.total))
    }
  }
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
  const total = ranked.reduce((sum, b) => sum + b.total, 0)

  // The ledger is increments and never a running total, so the curve is the
  // running sum of the ones that came back.
  const x: number[] = []
  const curve: Record<'realized' | 'fee' | 'funding' | 'net', number[]> = {
    realized: [],
    fee: [],
    funding: [],
    net: [],
  }
  const running: Record<PnlKind, number> = { realized: 0, fee: 0, funding: 0 }
  for (const increment of pnl.state === 'ready' ? pnl.data : []) {
    running[increment.kind] += toNumber(increment.amount)
    x.push(nanosToMillis(increment.ts_event))
    curve.realized.push(running.realized)
    curve.fee.push(running.fee)
    curve.funding.push(running.funding)
    curve.net.push(running.realized + running.fee + running.funding)
  }
  const net = curve.net.at(-1) ?? 0
  const curves = [
    { key: 'net', label: 'Net', color: kindColor['net'] as string, values: curve.net, area: true },
    {
      key: 'realized',
      label: 'Realized',
      color: kindColor['realized'] as string,
      values: curve.realized,
    },
    { key: 'fee', label: 'Fees', color: kindColor['fee'] as string, values: curve.fee },
    {
      key: 'funding',
      label: 'Funding',
      color: kindColor['funding'] as string,
      values: curve.funding,
    },
  ]
  // Two points make a line. One increment is a fact, not a curve.
  const drawn = pnl.state === 'ready' && x.length >= 2

  const pnlPlaceholder =
    pnl.state === 'failed'
      ? trouble(pnl.title, pnl.detail)
      : pnl.state === 'loading'
        ? 'Reading the ledger'
        : x.length === 0
          ? `The ledger has nothing in the last ${k_window_hours}h`
          : x.length < 2
            ? 'One increment in the window; a curve needs two'
            : undefined

  const splitPlaceholder =
    balances.state === 'failed'
      ? trouble(balances.title, balances.detail)
      : balances.state === 'loading'
        ? 'Asking the venues'
        : total === 0
          ? `No venue is reporting a ${k_denomination} balance`
          : undefined

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2 pt-0">
      <div className="grid shrink-0 grid-cols-1 items-start gap-2 xl:grid-cols-[minmax(0,1fr)_380px]">
        {/* NOT EQUITY. `/v1/pnl` is signed realized, fee and funding
            increments; nothing in this system persists a balance series, so a
            curve of what the desk was worth cannot be drawn and must not be
            labelled as though it had been. */}
        <ChartFrame
          title={'Cumulative P&L'}
          note={`Realized, fees and funding, summed over ${k_window_hours}h. Not equity — no balance series is kept anywhere.`}
          readout={
            drawn ? (
              <>
                <div
                  className={cn(
                    'num text-xl font-semibold tracking-[-0.015em]',
                    net >= 0 ? 'text-up' : 'text-down',
                  )}
                >
                  {signed(net)}
                </div>
                <div className="meta">
                  {k_denomination} · {k_window_hours}h
                </div>
              </>
            ) : undefined
          }
          legend={
            drawn
              ? curves.map((c) => ({
                  key: c.key,
                  label: c.label,
                  color: c.color,
                  value: signed(c.values.at(-1) ?? 0),
                  muted: kind !== null && kind !== c.key,
                }))
              : undefined
          }
          onLegendHover={setKind}
          placeholder={pnlPlaceholder}
          height={260}
        >
          {({ width, height }) => (
            <LinePlot x={x} series={curves} width={width} height={height} highlight={kind} />
          )}
        </ChartFrame>

        <ChartFrame
          title="Split by venue"
          height={228}
          legend={
            total > 0
              ? segments.map((s) => ({
                  key: s.key,
                  label: s.label,
                  color: s.color,
                  value: `${((s.value / total) * 100).toFixed(1)}%`,
                }))
              : undefined
          }
          onLegendHover={setHover}
          placeholder={splitPlaceholder}
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
          right={
            balances.state === 'ready' ? (
              <span className="meta num">
                {reporting.length} of {accounts.length} accounts reporting
              </span>
            ) : undefined
          }
        />
        <div className="min-h-0 flex-1 overflow-auto">
          {balances.state === 'loading' && <EmptyState title="Asking the venues" />}
          {balances.state === 'failed' && (
            <EmptyState title={balances.title} hint={balances.detail} />
          )}
          {balances.state === 'ready' && <BalanceRows accounts={balances.data} />}
        </div>
      </section>
    </div>
  )
}
