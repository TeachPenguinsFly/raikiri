import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Venue } from '@/components/shared/Venue'
import { api, type FundingBoardRow } from '@/api/client'
import { compact, nanosToMillis, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'
import { cn } from '@/lib/utils'

/** The perpetual·perpetual funding board.
 *
 *  TWO COLUMNS, NEVER ONE SCORE. The funding edge and the price spread are
 *  different quantities, and a table ranked on their sum would float a pair to
 *  the top precisely when its two perpetuals have drifted apart — which is a
 *  directional position, not an arbitrage. This ranks on funding and shows the
 *  entry cost beside it, so the reader weighs them rather than the table.
 *
 *  Every rate here is already on an eight-hour clock. Venues fund on different
 *  ones, and comparing the numbers they publish would rank an hourly venue as
 *  though it paid an eighth of what it does. */

/** Basis points, because that is the size these live at: 0.0015 over eight
 *  hours is 15 bp, and as a percent rounded for a table it is 0.15 — a column
 *  of leading zeros. */
const bps = (fraction: number) => (fraction * 10_000).toFixed(1)

/** Time to that venue's own next settlement. BOTH legs are shown because they
 *  differ, and an edge collected in nine minutes on one side and six hours on
 *  the other is not the same trade as one that settles together. */
function untilSettlement(nanos: string | undefined): string {
  if (nanos === undefined || nanos === '' || nanos === '0') return '—'
  const left = nanosToMillis(nanos) - Date.now()
  if (left <= 0) return 'due'
  const minutes = Math.floor(left / 60_000)
  if (minutes < 60) return `${minutes}m`
  return `${Math.floor(minutes / 60)}h${String(minutes % 60).padStart(2, '0')}`
}

function Row({ row }: { row: FundingBoardRow }) {
  const spread = row.price_spread_bp ?? 0
  return (
    <tr className="border-t border-line/40 hover:bg-bg-hi/40">
      <td className="px-3 py-1.5 font-medium text-fg">{row.symbol}</td>
      <td className="px-3 py-1.5">
        <Venue venue={row.long_leg.venue} />
      </td>
      <td className="px-3 py-1.5">
        <Venue venue={row.short_leg.venue} />
      </td>
      <td className="num px-3 py-1.5 text-right text-up">{bps(toNumber(row.funding_edge_8h))}</td>
      {/* Negative is what it COSTS to get on to the route, and the funding has
          to beat it before anything is earned. Painted as a loss because it is
          one. */}
      <td className={cn('num px-3 py-1.5 text-right', spread < 0 ? 'text-down' : 'text-fg-mid')}>
        {spread}
      </td>
      <td className="num px-3 py-1.5 text-right text-fg-dim">
        {untilSettlement(row.long_leg.next_funding_time)}
        <span className="text-fg-dim/50"> / </span>
        {untilSettlement(row.short_leg.next_funding_time)}
      </td>
      <td className="num px-3 py-1.5 text-right text-fg-dim">
        {compact(toNumber(row.volume_floor))}
      </td>
    </tr>
  )
}

export function ScannerPage() {
  const board = useQuery(() => api.fundingBoard(), [], 2000)

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex min-h-0 flex-1 flex-col">
        <PanelHeader
          title="Funding board"
          right={
            board.state === 'ready' ? (
              <span className="text-2xs text-fg-dim">
                {board.data.rows.length} of {board.data.considered} pairs, {board.data.venues} live
                venues
              </span>
            ) : undefined
          }
        />

        {board.state === 'loading' && <EmptyState title="Reading the segments…" />}

        {/* THE REFUSAL IS THE ANSWER. A scanner that is not running produces no
            board, and an empty table here would say the search ran and found
            nothing — a claim about the market rather than about us. */}
        {board.state === 'failed' && (
          <EmptyState title={board.title} hint={board.detail ?? undefined} />
        )}

        {board.state === 'ready' && board.data.rows.length === 0 && (
          <EmptyState
            title="No pair cleared the filters"
            hint={`${board.data.considered} priced across ${board.data.symbols} symbols`}
          />
        )}

        {board.state === 'ready' && board.data.rows.length > 0 && (
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full text-xs">
              <thead className="sticky top-0 bg-bg text-2xs uppercase tracking-wide text-fg-dim">
                <tr>
                  <th className="px-3 py-1.5 text-left font-normal">Symbol</th>
                  <th className="px-3 py-1.5 text-left font-normal">Hold</th>
                  <th className="px-3 py-1.5 text-left font-normal">Sell</th>
                  <th className="px-3 py-1.5 text-right font-normal">Funding 8h, bp</th>
                  <th className="px-3 py-1.5 text-right font-normal">Entry, bp</th>
                  <th className="px-3 py-1.5 text-right font-normal">Settles in</th>
                  <th className="px-3 py-1.5 text-right font-normal">Volume 24h</th>
                </tr>
              </thead>
              <tbody>
                {board.data.rows.map((row) => (
                  <Row
                    key={`${row.symbol}:${row.long_leg.venue}:${row.short_leg.venue}`}
                    row={row}
                  />
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>

      <section className="panel p-3 text-2xs leading-relaxed text-fg-dim">
        Gross edge, before fees and before depth. What is executable depends on
        size, on the account&apos;s fee tier and on the book past its top level,
        none of which the scanner reads. A leg whose venue does not publish its
        funding interval is left out rather than assumed to be on the common one.
      </section>
    </div>
  )
}
