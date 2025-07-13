import { useMemo, useState } from 'react'
import { allPnl, api, type AccountBalances, type PnlIncrement } from '@/api/client'
import { Failed, Loading } from '@/components/Chrome'
import { ChartFrame, colorFor, Donut, kindColor, LinePlot } from '@/components/chart'
import { cn, compact, nanosToMillis, percent, toNumber, usd, usdSigned } from '@/lib/format'
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
}

/** One asset held on one venue, flattened out of the per-account reply. */
interface Holding {
  venue: string
  account: string
  asset: string
  total: number
  available: number | null
  stale: boolean
  reported: boolean
}

/** The asset the desk is actually denominated in — the one the most venues
 *  report, ties broken by size. Chosen rather than assumed to be USDT, and
 *  named on screen, because a donut that silently added USDT to BTC would be a
 *  number nobody holds. */
function dominantAsset(holdings: Holding[]): string | null {
  const weight = new Map<string, { venues: number; total: number }>()
  for (const h of holdings) {
    const entry = weight.get(h.asset) ?? { venues: 0, total: 0 }
    entry.venues += 1
    entry.total += h.total
    weight.set(h.asset, entry)
  }
  const ranked = [...weight.entries()].sort(
    (a, b) => b[1].venues - a[1].venues || b[1].total - a[1].total,
  )
  return ranked[0]?.[0] ?? null
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

  // Balances are live from the gateway segments and move on the venue's own
  // 30 s pull, so they are polled faster than the ledger, which only moves when
  // something trades.
  const held = useQuery(() => api.balances(), [], 5_000)
  const ledger = useQuery(() => allPnl(), [], 15_000)
  const model = useMemo(() => {
    // Built from WHATEVER HAS ARRIVED. Balances come from shared memory and
    // the ledger from Postgres; they fail independently, and letting one
    // failure blank the other hides a number that was there all along.
    const increments = ledger.state === 'ready' ? ledger.data : []

    const byVenue = new Map<string, VenueRow>()
    const totals: Record<Kind, number> = { realized: 0, fee: 0, funding: 0 }

    for (const row of increments) {
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
      }
      entry.flow += Math.abs(amount)
      entry.net += amount
      entry.byKind[kind] += amount
      byVenue.set(venue, entry)
    }

    const venues = [...byVenue.values()].sort((a, b) => b.flow - a.flow)

    const holdings: Holding[] = []
    const silent: string[] = []
    if (held.state === 'ready') {
      for (const row of held.data as AccountBalances[]) {
        if (row.reported_at === null || row.reported_at === undefined) {
          // The venue has not answered. Listed by name, because "no balances"
          // and "an empty account" are the two things this page must not merge.
          silent.push(row.account)
          continue
        }
        for (const asset of row.assets) {
          holdings.push({
            venue: row.venue,
            account: row.account,
            asset: asset.asset,
            total: toNumber(asset.total),
            available: asset.available === undefined ? null : toNumber(asset.available),
            stale: row.stale,
            reported: true,
          })
        }
      }
    }
    const asset = dominantAsset(holdings)
    const denominated = holdings.filter((h) => h.asset === asset)

    return {
      venues,
      totals,
      net: totals.realized + totals.fee + totals.funding,
      flow: venues.reduce((sum, v) => sum + v.flow, 0),
      series: cumulative(increments, 400),
      holdings,
      silent,
      asset,
      denominated,
      capital: denominated.reduce((sum, h) => sum + h.total, 0),
    }
  }, [ledger, held])

  // Only a screen with nothing at all is a failed screen.
  if (held.state === 'loading' && ledger.state === 'loading') {
    return <Loading what="balances and the ledger" />
  }
  if (held.state === 'failed' && ledger.state === 'failed') return <Failed {...held} />

  const focused = model.denominated.find((h) => h.venue === hovered) ?? null

  return (
    <div className="h-full space-y-4 overflow-auto p-5">
      <Banner silent={model.silent} asset={model.asset} />

      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Tile
          label={model.asset === null ? 'capital' : `capital · ${model.asset}`}
          value={model.asset === null ? '—' : usd(model.capital)}
          tone={0}
          emphasis
        />
        <Tile label="Net" value={usdSigned(model.net)} tone={model.net} emphasis />
        <Tile label="realized" value={usdSigned(model.totals.realized)} tone={model.totals.realized} />
        <Tile label="fee" value={usdSigned(model.totals.fee)} tone={model.totals.fee} />
      </div>

      <div className="grid gap-4 xl:grid-cols-12">
        <ChartFrame
          title="Capital by exchange"
          note={
            model.asset === null
              ? 'No venue has answered with a balance yet.'
              : `${model.asset} as each venue reports it. Only one asset is summed — adding ${model.asset} to another coin would be a number nobody holds.`
          }
          height={296}
          className="xl:col-span-5"
          placeholder={model.denominated.length === 0 ? 'No balances reported yet.' : undefined}
          footer={
            model.silent.length === 0
              ? `${model.denominated.length} ${model.denominated.length === 1 ? 'exchange' : 'exchanges'} reporting`
              : `not reporting: ${model.silent.join(', ')}`
          }
        >
          {({ width, height }) => {
            const size = Math.min(height, Math.max(width * 0.46, 140))
            return (
              <div className="flex h-full items-center gap-5 px-2">
                <Donut
                  size={size}
                  segments={model.denominated.map((h) => ({
                    key: h.venue,
                    label: h.venue,
                    color: colorFor(h.venue),
                    value: h.total,
                  }))}
                  active={hovered}
                  onHover={setHovered}
                  center={
                    <>
                      <span className="text-[10px] uppercase tracking-[0.14em] text-faint">
                        {focused?.venue ?? model.asset ?? '—'}
                      </span>
                      <span className="tnum mt-0.5 text-xl text-ink">
                        {compact(focused?.total ?? model.capital)}
                      </span>
                      <span className="tnum text-xs">
                        {focused === null ? (
                          <span className="text-muted">
                            across {model.denominated.length}{' '}
                            {model.denominated.length === 1 ? 'venue' : 'venues'}
                          </span>
                        ) : (
                          <>
                            <span className="text-muted">
                              {percent(model.capital === 0 ? 0 : focused.total / model.capital)}
                            </span>
                            {focused.available !== null && focused.available !== focused.total && (
                              <>
                                <span className="text-faint"> · </span>
                                <span className="text-muted">
                                  {compact(focused.available)} free
                                </span>
                              </>
                            )}
                          </>
                        )}
                      </span>
                    </>
                  }
                />
                <ul className="min-w-0 flex-1 space-y-0.5 overflow-auto" style={{ maxHeight: height }}>
                  {model.denominated.map((holding) => (
                    <li
                      key={holding.account}
                      onMouseEnter={() => setHovered(holding.venue)}
                      onMouseLeave={() => setHovered(null)}
                      className={cn(
                        'flex items-center justify-between gap-2 rounded px-2 py-1 text-xs transition-colors',
                        hovered === holding.venue ? 'bg-raised' : 'bg-transparent',
                      )}
                    >
                      <span className="flex min-w-0 items-center gap-2">
                        <span
                          className="size-2 shrink-0 rounded-full"
                          style={{ backgroundColor: colorFor(holding.venue) }}
                        />
                        <span className="truncate text-ink">{holding.venue}</span>
                        {holding.stale && (
                          <span className="shrink-0 text-[10px] uppercase text-warn">stale</span>
                        )}
                      </span>
                      <span className="flex shrink-0 items-center gap-3">
                        <span className="tnum w-11 text-right text-muted">
                          {percent(model.capital === 0 ? 0 : holding.total / model.capital)}
                        </span>
                        <span className="tnum w-24 text-right text-ink">{usd(holding.total)}</span>
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
            ledger.state === 'failed'
              ? `${ledger.title} — the balances above are unaffected`
              : model.series.x.length < 2
                ? 'Fewer than two increments — nothing to plot yet.'
                : undefined
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
              <th className="px-4 py-2 text-right font-normal">Held</th>
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
                  {(() => {
                    const rows = model.holdings.filter((h) => h.venue === venue.venue)
                    if (rows.length === 0) return '—'
                    return rows.map((h) => `${compact(h.total)} ${h.asset}`).join(' · ')
                  })()}
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

/** Shown only when something on this page is missing, and naming what. A
 *  standing banner is one a reader stops seeing. */
function Banner({ silent, asset }: { silent: string[]; asset: string | null }) {
  if (silent.length === 0 && asset !== null) {
    return null
  }
  return (
    <div className="flex items-start gap-3 rounded-xl border border-warn/25 bg-warn/[0.06] px-4 py-3">
      <span className="mt-0.5 size-1.5 shrink-0 rounded-full bg-warn" />
      <p className="text-xs leading-relaxed text-muted">
        {asset === null ? (
          <span className="text-ink">No venue has answered with a balance yet.</span>
        ) : (
          <>
            <span className="text-ink">
              {silent.join(', ')} {silent.length === 1 ? 'reports' : 'report'} no balance.
            </span>{' '}
            The capital above is what the rest of them hold, so it is a floor and not a total.
          </>
        )}{' '}
        <span className="text-faint">
          Funding is still the published rate everywhere and has moved no money, so a funding
          column of zero is honest rather than quiet.
        </span>
      </p>
    </div>
  )
}
