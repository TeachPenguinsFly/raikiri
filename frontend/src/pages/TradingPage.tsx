import { useState } from 'react'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { VenueTag } from '@/components/shared/VenueTag'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { ORDERS, POSITIONS, useBook, useGateways } from '@/mock/data'

const usd = (v: number) =>
  v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })

/** The desk: what is on, what is working, and somewhere to act on it.
 *
 *  Markets is where a market is chosen; this is where the book already open is
 *  managed. They share the blotter and the ticket rather than each growing
 *  their own. */
export function TradingPage() {
  const [picked, setPicked] = useState<string | null>(null)
  const gateways = useGateways()
  const entering = useEntranceOnce(true, 'trading')

  const position =
    POSITIONS.find((p) => `${p.venue}:${p.symbol}` === picked) ?? POSITIONS[0] ?? null

  const book = useBook(position?.mark ?? null, 20)

  const gross = POSITIONS.reduce((sum, p) => sum + Math.abs(p.qty) * p.mark, 0)
  const net = POSITIONS.reduce((sum, p) => sum + p.qty * p.mark, 0)
  const unrealized = POSITIONS.reduce((sum, p) => sum + (p.mark - p.entry) * p.qty, 0)
  // NULL IS NOT FLAT: a venue with no position endpoint is unverified, not
  // agreed, and the count of those is the number worth watching here.
  const unverified = POSITIONS.filter((p) => p.venueQty === null).length

  return (
    <div className="flex h-full min-h-0 gap-2 p-2 pt-0">
      <div className="flex min-w-0 flex-1 flex-col gap-2">
        <div className="grid shrink-0 grid-cols-2 gap-2 lg:grid-cols-5">
          <Stat label="Gross exposure" value={usd(gross)} />
          <Stat label="Net exposure" value={usd(net)} tone={net >= 0 ? 'up' : 'down'} />
          <Stat
            label="Unrealized"
            value={`${unrealized >= 0 ? '+' : '−'}${usd(Math.abs(unrealized))}`}
            tone={unrealized >= 0 ? 'up' : 'down'}
          />
          <Stat label="Working orders" value={String(ORDERS.length)} />
          <Stat
            label="Unverified"
            value={`${unverified} of ${POSITIONS.length}`}
            tone={unverified > 0 ? 'amber' : 'up'}
            note="venues with no position endpoint"
          />
        </div>

        <section className="panel flex max-h-[55%] shrink-0 flex-col">
          <PanelHeader
            title="Positions"
            right={
              <span className="micro-label">
                {gateways.filter((g) => g.alive).length}/{gateways.length} gateways alive
              </span>
            }
          />
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-collapse text-[11.5px]">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="border-b border-line text-3xs uppercase tracking-[0.08em] text-fg-dim">
                  <th className="h-7 px-2.5 text-left font-medium">Venue</th>
                  <th className="h-7 px-2 text-left font-medium">Symbol</th>
                  <th className="h-7 px-2 text-left font-medium">Side</th>
                  <th className="h-7 px-2 text-right font-medium">Qty</th>
                  <th className="h-7 px-2 text-right font-medium">Entry</th>
                  <th className="h-7 px-2 text-right font-medium">Mark</th>
                  <th className="h-7 px-2 text-right font-medium">Notional</th>
                  <th className="h-7 px-2.5 text-right font-medium">Unrealized</th>
                </tr>
              </thead>
              <tbody>
                {POSITIONS.map((p, i) => {
                  const key = `${p.venue}:${p.symbol}`
                  const pnl = (p.mark - p.entry) * p.qty
                  return (
                    <tr
                      key={key}
                      style={entering ? enterDelay(i) : undefined}
                      onClick={() => setPicked(key)}
                      className={cn(
                        'cursor-pointer border-b border-line/40 transition-colors duration-75',
                        entering && 'enter-fade-up',
                        key === `${position?.venue}:${position?.symbol}`
                          ? 'bg-accent/8 shadow-[inset_2px_0_0_0_var(--color-accent)]'
                          : 'hover:bg-hover',
                      )}
                    >
                      <td className="px-2.5 py-1.5">
                        <VenueTag venue={p.venue} full />
                      </td>
                      <td className="px-2 py-1.5 font-mono text-3xs text-fg-mid">{p.symbol}</td>
                      <td className="px-2 py-1.5">
                        <StatusBadge status={p.qty >= 0 ? 'long' : 'short'} />
                      </td>
                      <td className={cn('num px-2 py-1.5 text-right', p.qty >= 0 ? 'text-up' : 'text-down')}>
                        {p.qty > 0 ? '+' : ''}
                        {p.qty}
                      </td>
                      <td className="num px-2 py-1.5 text-right text-fg-mid">{usd(p.entry)}</td>
                      <td className="num px-2 py-1.5 text-right">{usd(p.mark)}</td>
                      <td className="num px-2 py-1.5 text-right text-fg-mid">
                        {usd(Math.abs(p.qty) * p.mark)}
                      </td>
                      <td className={cn('num px-2.5 py-1.5 text-right', pnl >= 0 ? 'text-up' : 'text-down')}>
                        {pnl >= 0 ? '+' : '−'}
                        {usd(Math.abs(pnl))}
                      </td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
        </section>

        <div className="flex min-h-0 flex-1 flex-col">
          <Blotter tabs={['orders', 'trades', 'balances']} />
        </div>
      </div>

      <div className="flex w-[268px] shrink-0 flex-col gap-2">
        <OrderTicket
          venue={position?.venue ?? null}
          symbol={position?.symbol ?? null}
          bid={position === null ? null : position.mark * 0.9999}
          ask={position === null ? null : position.mark * 1.0001}
        />
        {/* The book of the position you have selected, so closing it is one
            glance rather than a trip back to Markets. */}
        <section className="panel flex min-h-0 flex-1 flex-col">
          <PanelHeader
            title="Depth"
            right={position === null ? null : <VenueTag venue={position.venue} full />}
          />
          <Ladder bids={book.bids} asks={book.asks} />
        </section>
      </div>
    </div>
  )
}

function Stat({
  label,
  value,
  tone,
  note,
}: {
  label: string
  value: string
  tone?: 'up' | 'down' | 'amber'
  note?: string
}) {
  return (
    <div className="panel px-3 py-2">
      <div className="text-3xs uppercase tracking-[0.08em] text-fg-dim">{label}</div>
      <div
        className={cn(
          'num mt-0.5 text-[15px] font-bold',
          tone === 'up' && 'text-up',
          tone === 'down' && 'text-down',
          tone === 'amber' && 'text-amber',
          tone === undefined && 'text-fg',
        )}
      >
        {value}
      </div>
      {note !== undefined && <div className="mt-0.5 text-3xs text-fg-dim">{note}</div>}
    </div>
  )
}
