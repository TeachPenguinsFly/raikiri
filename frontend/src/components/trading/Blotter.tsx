import { useState, type ReactNode } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { VenueTag } from '@/components/shared/VenueTag'
import { cn } from '@/lib/utils'
import { BALANCES, ORDERS, POSITIONS, TRADES } from '@/mock/data'

export type Tab = 'orders' | 'trades' | 'positions' | 'balances'

const k_all: Tab[] = ['orders', 'trades', 'positions', 'balances']

const num = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })

/** The bottom of the screen, where what you have done lives.
 *
 *  Tabbed rather than four panels: they answer one question at four distances —
 *  what is working, what filled, what is held, what is left — and a trader is
 *  in one of those at a time. The COUNT stays on every tab so a fill landing on
 *  a tab you are not looking at is still visible.
 *
 *  `tabs` narrows it. A screen that already shows positions in full does not
 *  want them a second time three inches lower. */
export function Blotter({ tabs = k_all }: { tabs?: Tab[] }) {
  const [tab, setTab] = useState<Tab>(tabs[0] as Tab)

  const counts: Record<Tab, number> = {
    orders: ORDERS.length,
    trades: TRADES.length,
    positions: POSITIONS.length,
    balances: BALANCES.filter((b) => b.reported).length,
  }

  return (
    <section className="panel flex min-h-0 flex-1 flex-col">
      {/* The active tab is marked by weight and a two-pixel rule, not by a
          filled capsule: four capsules in a row is a segmented control, and
          this is navigation between views. */}
      <div className="flex h-11 shrink-0 items-center gap-5 px-3">
        {tabs.map((t) => (
          <button
            key={t}
            type="button"
            onClick={() => setTab(t)}
            className={cn(
              'relative flex h-11 items-center gap-1.5 text-sm capitalize transition-colors duration-150',
              tab === t
                ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                : 'text-fg-dim hover:text-fg-mid',
            )}
          >
            {t}
            <span className={cn('num text-2xs', tab === t ? 'text-fg-mid' : 'text-fg-dim')}>
              {counts[t]}
            </span>
          </button>
        ))}
      </div>

      <div className="min-h-0 flex-1 overflow-auto">
        {tab === 'orders' && <Orders />}
        {tab === 'trades' && <Trades />}
        {tab === 'positions' && <Positions />}
        {tab === 'balances' && <Balances />}
      </div>
    </section>
  )
}

/** A table with one hairline in it: the one under the header.
 *
 *  Rows are separated by rhythm and by hover, not by rules. Twelve rules in a
 *  twelve-row table is a grid, and a grid competes with the figures it holds. */
function Grid({ head, children }: { head: ReactNode; children: ReactNode }) {
  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">{head}</tr>
      </thead>
      <tbody>{children}</tbody>
    </table>
  )
}

const Th = ({ children, right }: { children: ReactNode; right?: boolean }) => (
  <th className={cn('col-head px-3 font-medium', right === true ? 'text-right' : 'text-left')}>
    {children}
  </th>
)

/** Cell shorthands, so the alignment and the colour of a kind of value are
 *  decided once rather than per column. */
const Id = ({ children }: { children: ReactNode }) => (
  <td className="px-3 font-mono text-2xs text-fg-dim">{children}</td>
)
const Sym = ({ children }: { children: ReactNode }) => (
  <td className="px-3 font-mono text-xs text-fg-mid">{children}</td>
)
const N = ({
  children,
  className,
}: {
  children: ReactNode
  className?: string
}) => <td className={cn('num px-3 text-right text-fg', className)}>{children}</td>

function Orders() {
  if (ORDERS.length === 0) return <EmptyState title="Nothing working" />
  return (
    <Grid
      head={
        <>
          <Th>Order</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th>Type</Th>
          <Th right>Price</Th>
          <Th right>Filled</Th>
          <Th>Status</Th>
          <Th right>Age</Th>
        </>
      }
    >
      {ORDERS.map((o) => (
        <tr key={o.id} className="row">
          <Id>{o.id}</Id>
          <td className="px-3">
            <VenueTag venue={o.venue} full />
          </td>
          <Sym>{o.symbol}</Sym>
          <td className="px-3">
            <StatusBadge status={o.side} />
          </td>
          <td className="px-3 text-fg-mid capitalize">{o.type}</td>
          <N>{o.price === null ? '—' : num(o.price, 1)}</N>
          {/* Filled shown against the order, not alone: "0.1" answers nothing
              without the 0.25 it is a tenth of. */}
          <N>
            <span className={o.filled > 0 ? 'text-amber' : 'text-fg-dim'}>{o.filled}</span>
            <span className="text-fg-dim"> / {o.qty}</span>
          </N>
          <td className="px-3">
            <StatusBadge status={o.status} />
          </td>
          <td className="num px-3 text-right text-xs text-fg-dim">{o.age}</td>
        </tr>
      ))}
    </Grid>
  )
}

function Trades() {
  return (
    <Grid
      head={
        <>
          <Th>Execution</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th right>Price</Th>
          <Th right>Qty</Th>
          <Th right>Fee</Th>
          <Th>Liquidity</Th>
          <Th right>Time</Th>
        </>
      }
    >
      {TRADES.map((t) => (
        <tr key={t.id} className="row">
          <Id>{t.id}</Id>
          <td className="px-3">
            <VenueTag venue={t.venue} full />
          </td>
          <Sym>{t.symbol}</Sym>
          <td className="px-3">
            <StatusBadge status={t.side} />
          </td>
          <N>{num(t.price, 1)}</N>
          <N>{t.qty}</N>
          <td className="num px-3 text-right text-fg-mid">{t.fee.toFixed(4)}</td>
          <td className="px-3 text-fg-mid">{t.liquidity}</td>
          <td className="num px-3 text-right text-xs text-fg-dim">{t.at}</td>
        </tr>
      ))}
    </Grid>
  )
}

function Positions() {
  return (
    <Grid
      head={
        <>
          <Th>Instrument</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th right>Qty</Th>
          <Th right>Entry</Th>
          <Th right>Mark</Th>
          <Th right>Unrealized</Th>
          <Th right>Venue says</Th>
        </>
      }
    >
      {POSITIONS.map((p) => {
        const pnl = (p.mark - p.entry) * p.qty
        const agrees = p.venueQty !== null && Math.abs(p.venueQty - p.qty) < 1e-9
        return (
          <tr key={`${p.venue}:${p.symbol}`} className="row">
            <Id>{p.instrumentId}</Id>
            <td className="px-3">
              <VenueTag venue={p.venue} full />
            </td>
            <Sym>{p.symbol}</Sym>
            <N className={p.qty >= 0 ? 'text-up' : 'text-down'}>
              {p.qty > 0 ? '+' : ''}
              {p.qty}
            </N>
            <td className="num px-3 text-right text-fg-mid">{num(p.entry, 1)}</td>
            <N>{num(p.mark, 1)}</N>
            <N className={pnl >= 0 ? 'text-up' : 'text-down'}>
              {pnl >= 0 ? '+' : '−'}
              {num(Math.abs(pnl))}
            </N>
            {/* NULL IS NOT FLAT. A venue that cannot report and a venue
                reporting zero are different facts, and only one of them means
                the position is gone. */}
            <td className="num px-3 text-right text-xs">
              {p.venueQty === null ? (
                <span className="text-fg-dim" title="this venue reports no position endpoint">
                  not reported
                </span>
              ) : agrees ? (
                <span className="text-fg-mid">agrees</span>
              ) : (
                <span className="text-down">{p.venueQty}</span>
              )}
            </td>
          </tr>
        )
      })}
    </Grid>
  )
}

function Balances() {
  const reported = BALANCES.filter((b) => b.reported)
  const total = reported.filter((b) => b.asset === 'USDT').reduce((sum, b) => sum + b.total, 0)

  return (
    <Grid
      head={
        <>
          <Th>Venue</Th>
          <Th>Asset</Th>
          <Th right>Total</Th>
          <Th right>Available</Th>
        </>
      }
    >
      {reported.map((b) => (
        <tr key={`${b.account}:${b.asset}`} className="row">
          <td className="px-3">
            <VenueTag venue={b.venue} full />
          </td>
          <td className="px-3 text-fg-mid">{b.asset}</td>
          <N>{num(b.total, b.total < 1 ? 4 : 2)}</N>
          {/* A venue that publishes a total and no free figure leaves a dash.
              Repeating the total there invites committing margin twice. */}
          <N className="text-fg-mid">
            {b.available === null ? (
              <span className="text-fg-dim" title="this venue publishes no per-asset free figure">
                —
              </span>
            ) : (
              num(b.available, b.available < 1 ? 4 : 2)
            )}
          </N>
        </tr>
      ))}
      <tr className="h-8 shadow-[inset_0_1px_0_0_var(--color-line)]">
        <td colSpan={2} className="px-3 text-xs text-fg-dim">
          USDT across venues
        </td>
        <N className="font-medium">{num(total)}</N>
        <td />
      </tr>
    </Grid>
  )
}
