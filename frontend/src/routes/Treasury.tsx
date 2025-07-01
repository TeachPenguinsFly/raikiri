import { useMemo, useState } from 'react'
import { allPnl, api, type PnlIncrement } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { ChartFrame, colorFor, Donut, kindColor, LinePlot } from '@/components/chart'
import { cn, compact, nanosToMillis, percent, toNumber, usdSigned } from '@/lib/format'
import { useQuery } from '@/lib/query'

const KINDS = ['realized', 'fee', 'funding'] as const
type Kind = (typeof KINDS)[number]

interface VenueRow {
  venue: string
  color: string
  /** Absolute money moved. Non-negative, which is what a proportion needs. */
  flow: number
  net: number
  byKind: Record<Kind, number>
  positions: number
}

/** Cumulative series, thinned to something a screen can draw.
 *
 *  Every point is a real increment, never an interpolation: the ledger is a
 *  sequence of events and a synthetic midpoint would put money on the chart at
 *  a moment nothing happened. Thinning drops points, it does not invent them. */
function cumulative(rows: PnlIncrement[], limit: number) {
  const running: Record<Kind, number> = { realized: 0, fee: 0, funding: 0 }
  const x: number[] = []
  const values: Record<Kind | 'net', number[]> = { realized: [], fee: [], funding: [], net: [] }
  const stride = Math.max(1, Math.ceil(rows.length / limit))

  rows.forEach((row, index) => {
    const kind = row.kind as Kind
    if (KINDS.includes(kind)) running[kind] += toNumber(row.amount)
    if (index % stride !== 0 && index !== rows.length - 1) return
    x.push(nanosToMillis(row.ts_event))
    for (const kind_ of KINDS) values[kind_].push(running[kind_])
    values.net.push(running.realized + running.fee + running.funding)
  })

  return { x, values }
}

export function Treasury() {
  const [hovered, setHovered] = useState<string | null>(null)

  const ledger = useQuery(() => allPnl(), [], 15_000)
  const held = useQuery(
    async () => {
      const accounts = await api.accounts()
      const counts = await Promise.all(
        accounts.map(async (account) => ({
          venue: account.venue,
          rows: (await api.positions({ account: account.account })).length,
        })),
      )
      return counts
    },
    [],
    15_000,
  )

  const model = useMemo(() => {
    if (ledger.state !== 'ready') return null

    const byVenue = new Map<string, VenueRow>()
    const totals: Record<Kind, number> = { realized: 0, fee: 0, funding: 0 }

    for (const row of ledger.data) {
      const kind = row.kind as Kind
      if (!KINDS.includes(kind)) continue
      const amount = toNumber(row.amount)
      totals[kind] += amount

      // The ledger's `account` is written as the venue that charged the money;
      // it is not the (venue, market) account a gateway runs under. Grouping by
      // it is therefore a grouping by exchange, which is what this page wants.
      const venue = row.account
      const entry = byVenue.get(venue) ?? {
        venue,
        color: colorFor(venue),
        flow: 0,
        net: 0,
        byKind: { realized: 0, fee: 0, funding: 0 },
        positions: 0,
      }
      entry.flow += Math.abs(amount)
      entry.net += amount
      entry.byKind[kind] += amount
      byVenue.set(venue, entry)
    }

    if (held.state === 'ready') {
      for (const row of held.data) {
        const entry = byVenue.get(row.venue)
        if (entry !== undefined) entry.positions += row.rows
      }
    }

    const venues = [...byVenue.values()].sort((a, b) => b.flow - a.flow)
    return {
      venues,
      totals,
      net: totals.realized + totals.fee + totals.funding,
      flow: venues.reduce((sum, v) => sum + v.flow, 0),
      series: cumulative(ledger.data, 400),
    }
  }, [ledger, held])

  if (ledger.state === 'loading') return <Loading what="the ledger" />
  if (ledger.state === 'failed') return <Failed {...ledger} />
  if (model === null) return null

  const focused = model.venues.find((v) => v.venue === hovered) ?? null

  return (
    <div className="h-full space-y-4 overflow-auto p-5">
      <Banner />

      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Tile label="Net" value={usdSigned(model.net)} tone={model.net} emphasis />
        {KINDS.map((kind) => (
          <Tile key={kind} label={kind} value={usdSigned(model.totals[kind])} tone={model.totals[kind]} />
        ))}
      </div>

      <div className="grid gap-4 xl:grid-cols-12">
        <ChartFrame
          title="Split by exchange"
          note="Absolute money moved through each venue — realised PnL, fees paid, funding taken."
          height={296}
          className="xl:col-span-5"
          placeholder={model.venues.length === 0 ? 'The ledger is empty.' : undefined}
          footer={`${model.venues.length} ${model.venues.length === 1 ? 'exchange' : 'exchanges'} in the ledger`}
        >
          {({ width, height }) => {
            const size = Math.min(height, Math.max(width * 0.46, 140))
            return (
              <div className="flex h-full items-center gap-5 px-2">
                <Donut
                  size={size}
                  segments={model.venues.map((v) => ({
                    key: v.venue,
                    label: v.venue,
                    color: v.color,
                    value: v.flow,
                  }))}
                  active={hovered}
                  onHover={setHovered}
                  center={
                    <>
                      <span className="text-[10px] uppercase tracking-[0.14em] text-faint">
                        {focused?.venue ?? 'total flow'}
                      </span>
                      <span className="tnum mt-0.5 text-xl text-ink">
                        ${compact(focused?.flow ?? model.flow)}
                      </span>
                      <span className="tnum text-xs">
                        {focused === null ? (
                          <span className={toneClass(model.net)}>net {usdSigned(model.net)}</span>
                        ) : (
                          <>
                            <span className="text-muted">
                              {percent(model.flow === 0 ? 0 : focused.flow / model.flow)}
                            </span>
                            <span className="text-faint"> · </span>
                            <span className={toneClass(focused.net)}>{usdSigned(focused.net)}</span>
                          </>
                        )}
                      </span>
                    </>
                  }
                />
                <ul className="min-w-0 flex-1 space-y-0.5 overflow-auto" style={{ maxHeight: height }}>
                  {model.venues.map((venue) => (
                    <li
                      key={venue.venue}
                      onMouseEnter={() => setHovered(venue.venue)}
                      onMouseLeave={() => setHovered(null)}
                      className={cn(
                        'flex items-center justify-between gap-2 rounded px-2 py-1 text-xs transition-colors',
                        hovered === venue.venue ? 'bg-raised' : 'bg-transparent',
                      )}
                    >
                      <span className="flex min-w-0 items-center gap-2">
                        <span
                          className="size-2 shrink-0 rounded-full"
                          style={{ backgroundColor: venue.color }}
                        />
                        <span className="truncate text-ink">{venue.venue}</span>
                      </span>
                      <span className="flex shrink-0 items-center gap-3">
                        <span className="tnum w-11 text-right text-muted">
                          {percent(model.flow === 0 ? 0 : venue.flow / model.flow)}
                        </span>
                        <span className={cn('tnum w-20 text-right', toneClass(venue.net))}>
                          {usdSigned(venue.net)}
                        </span>
                      </span>
                    </li>
                  ))}
                </ul>
              </div>
            )
          }}
        </ChartFrame>

        <ChartFrame
          title="Cumulative ledger"
          note="Increments summed forward. Not equity — nothing here counts a balance."
          height={296}
          className="xl:col-span-7"
          readout={
            <>
              <div className={cn('tnum text-2xl', toneClass(model.net))}>{usdSigned(model.net)}</div>
              <div className="text-[11px] text-faint">{model.series.x.length} points</div>
            </>
          }
          legend={[
            { key: 'net', label: 'net', color: kindColor.net as string, value: usdSigned(model.net) },
            ...KINDS.map((kind) => ({
              key: kind,
              label: kind,
              color: kindColor[kind] as string,
              value: usdSigned(model.totals[kind]),
            })),
          ]}
          onLegendHover={setHovered}
          placeholder={
            model.series.x.length < 2 ? 'Fewer than two increments — nothing to plot yet.' : undefined
          }
        >
          {({ width, height }) => (
            <LinePlot
              width={width}
              height={height}
              x={model.series.x}
              highlight={hovered}
              series={[
                {
                  key: 'net',
                  label: 'net',
                  color: kindColor.net as string,
                  values: model.series.values.net,
                  area: true,
                },
                ...KINDS.map((kind) => ({
                  key: kind,
                  label: kind,
                  color: kindColor[kind] as string,
                  values: model.series.values[kind],
                })),
              ]}
            />
          )}
        </ChartFrame>
      </div>

      <section className="overflow-hidden rounded-xl border border-line bg-surface/80">
        <table className="w-full text-sm">
          <thead className="text-left text-[11px] uppercase tracking-[0.12em] text-faint">
            <tr className="border-b border-line">
              <th className="px-4 py-2 font-normal">Exchange</th>
              <th className="px-4 py-2 text-right font-normal">Share</th>
              <th className="px-4 py-2 text-right font-normal">Open positions</th>
              {KINDS.map((kind) => (
                <th key={kind} className="px-4 py-2 text-right font-normal">
                  {kind}
                </th>
              ))}
              <th className="px-4 py-2 text-right font-normal">Net</th>
            </tr>
          </thead>
          <tbody>
            {model.venues.map((venue) => (
              <tr
                key={venue.venue}
                onMouseEnter={() => setHovered(venue.venue)}
                onMouseLeave={() => setHovered(null)}
                className={cn(
                  'border-t border-line/50 transition-colors',
                  hovered === venue.venue ? 'bg-raised/60' : 'bg-transparent',
                )}
              >
                <td className="px-4 py-2">
                  <span className="flex items-center gap-2">
                    <span
                      className="size-2 rounded-full"
                      style={{ backgroundColor: venue.color }}
                    />
                    {venue.venue}
                  </span>
                </td>
                <td className="px-4 py-2 text-right">
                  <span className="flex items-center justify-end gap-2">
                    <span className="h-1 w-16 overflow-hidden rounded-full bg-line">
                      <span
                        className="block h-full rounded-full transition-[width] duration-700"
                        style={{
                          width: percent(model.flow === 0 ? 0 : venue.flow / model.flow),
                          backgroundColor: venue.color,
                        }}
                      />
                    </span>
                    <span className="tnum w-12 text-muted">
                      {percent(model.flow === 0 ? 0 : venue.flow / model.flow)}
                    </span>
                  </span>
                </td>
                <td className="tnum px-4 py-2 text-right text-muted">
                  {held.state === 'ready' ? venue.positions : '—'}
                </td>
                {KINDS.map((kind) => (
                  <td key={kind} className={cn('tnum px-4 py-2 text-right', toneClass(venue.byKind[kind]))}>
                    {usdSigned(venue.byKind[kind])}
                  </td>
                ))}
                <td className={cn('tnum px-4 py-2 text-right font-medium', toneClass(venue.net))}>
                  {usdSigned(venue.net)}
                </td>
              </tr>
            ))}
            {model.venues.length === 0 && (
              <tr>
                <td colSpan={7} className="px-4 py-6 text-center text-sm text-muted">
                  The ledger holds no increments. Run a strategy, or replay a journal into Postgres.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </section>
    </div>
  )
}

function toneClass(value: number): string {
  if (value > 0) return 'text-up'
  if (value < 0) return 'text-down'
  return 'text-muted'
}

function Tile({
  label,
  value,
  tone,
  emphasis,
}: {
  label: string
  value: string
  tone: number
  emphasis?: boolean
}) {
  return (
    <div className="rounded-xl border border-line bg-surface/80 px-4 py-3">
      <div className="text-[10px] uppercase tracking-[0.14em] text-faint">{label}</div>
      <div className={cn('tnum mt-1', emphasis === true ? 'text-2xl' : 'text-lg', toneClass(tone))}>
        {value}
      </div>
    </div>
  )
}

/** What this page is NOT, said once and in place rather than left to be
 *  inferred from a number that looks like a balance. */
function Banner() {
  return (
    <div className="flex items-start gap-3 rounded-xl border border-warn/25 bg-warn/[0.06] px-4 py-3">
      <span className="mt-0.5 size-1.5 shrink-0 rounded-full bg-warn" />
      <p className="text-xs leading-relaxed text-muted">
        <span className="text-ink">No venue reports balances yet.</span> No adapter reads an
        account endpoint, so nothing here is free capital or equity — it is the settled ledger,
        split by the exchange that charged it. When balances land they fill this same donut without
        changing its shape.{' '}
        <span className="text-faint">
          Funding is the published rate on every venue and has moved no money, so a funding column
          of zero is honest rather than quiet.
        </span>
      </p>
    </div>
  )
}
