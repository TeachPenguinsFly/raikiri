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
      <div className="flex h-8 shrink-0 items-center gap-0.5 border-b border-line px-1.5">
        {tabs.map((t) => (
          <button
            key={t}
            type="button"
            onClick={() => setTab(t)}
            className={cn(
              'flex h-6 items-center gap-1.5 rounded-[3px] px-2.5 text-2xs font-semibold capitalize transition-colors',
              tab === t ? 'bg-white/8 text-fg' : 'text-fg-dim hover:bg-white/4 hover:text-fg-mid',
            )}
          >
            {t}
            <span className={cn('num text-3xs', tab === t ? 'text-fg-mid' : 'text-fg-dim')}>
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

function Grid({ head, children }: { head: ReactNode; children: ReactNode }) {
  return (
    <table className="w-full border-collapse text-[12.5px]">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="border-b border-line text-3xs uppercase tracking-[0.06em] text-fg-mid">
          {head}
        </tr>
      </thead>
      <tbody>{children}</tbody>
    </table>
  )
}

const Th = ({ children, right }: { children: ReactNode; right?: boolean }) => (
  <th className={cn('h-7 px-2 font-medium', right === true ? 'text-right' : 'text-left')}>
    {children}
  </th>
)

function Orders() {
  if (ORDERS.length === 0) return <EmptyState title="Nothing working" />
  return (
    <Grid
      head={
        <>
          <Th>Order id</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th>Type</Th>
          <Th right>Price</Th>
          <Th right>Qty</Th>
          <Th right>Filled</Th>
          <Th>Status</Th>
          <Th right>Age</Th>
        </>
      }
    >
      {ORDERS.map((o) => (
        <tr key={o.id} className="row-hover border-b border-line/40">
          <td className="px-2 py-1.5 font-mono text-3xs text-fg-mid">{o.id}</td>
          <td className="px-2 py-1.5">
            <VenueTag venue={o.venue} full />
          </td>
          <td className="px-2 py-1.5 font-mono text-3xs text-fg">{o.symbol}</td>
          <td className="px-2 py-1.5">
            <StatusBadge status={o.side} />
          </td>
          <td className="px-2 py-1.5 text-fg-mid">{o.type}</td>
          <td className="num px-2 py-1.5 text-right">{o.price === null ? '—' : num(o.price, 1)}</td>
          <td className="num px-2 py-1.5 text-right text-fg">{o.qty}</td>
          {/* Filled shown against the order, not alone: "0.1" answers nothing
              without the 0.25 it is a tenth of. */}
          <td className="num px-2 py-1.5 text-right">
            <span className={o.filled > 0 ? 'text-amber' : 'text-fg-dim'}>{o.filled}</span>
            <span className="text-fg-dim"> / {o.qty}</span>
          </td>
          <td className="px-2 py-1.5">
            <StatusBadge status={o.status} />
          </td>
          <td className="num px-2 py-1.5 text-right text-fg-mid">{o.age}</td>
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
          <Th>Exec id</Th>
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
        <tr key={t.id} className="row-hover border-b border-line/40">
          <td className="px-2 py-1.5 font-mono text-3xs text-fg-mid">{t.id}</td>
          <td className="px-2 py-1.5">
            <VenueTag venue={t.venue} full />
          </td>
          <td className="px-2 py-1.5 font-mono text-3xs text-fg">{t.symbol}</td>
          <td className="px-2 py-1.5">
            <StatusBadge status={t.side} />
          </td>
          <td className="num px-2 py-1.5 text-right">{num(t.price, 1)}</td>
          <td className="num px-2 py-1.5 text-right text-fg">{t.qty}</td>
          <td className="num px-2 py-1.5 text-right text-fg-mid">{t.fee.toFixed(4)}</td>
          <td className="px-2 py-1.5">
            <StatusBadge status={t.liquidity} />
          </td>
          <td className="num px-2 py-1.5 text-right text-fg-mid">{t.at}</td>
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
          <tr key={`${p.venue}:${p.symbol}`} className="row-hover border-b border-line/40">
            <td className="px-2 py-1.5 font-mono text-3xs text-fg-mid">{p.instrumentId}</td>
            <td className="px-2 py-1.5">
              <VenueTag venue={p.venue} full />
            </td>
            <td className="px-2 py-1.5 font-mono text-3xs text-fg">{p.symbol}</td>
            <td className={cn('num px-2 py-1.5 text-right', p.qty >= 0 ? 'text-up' : 'text-down')}>
              {p.qty > 0 ? '+' : ''}
              {p.qty}
            </td>
            <td className="num px-2 py-1.5 text-right text-fg">{num(p.entry, 1)}</td>
            <td className="num px-2 py-1.5 text-right">{num(p.mark, 1)}</td>
            <td className={cn('num px-2 py-1.5 text-right', pnl >= 0 ? 'text-up' : 'text-down')}>
              {pnl >= 0 ? '+' : '−'}
              {num(Math.abs(pnl))}
            </td>
            {/* NULL IS NOT FLAT. A venue that cannot report and a venue
                reporting zero are different facts, and only one of them means
                the position is gone. */}
            <td className="num px-2 py-1.5 text-right">
              {p.venueQty === null ? (
                <span className="text-fg-dim" title="this venue reports no position endpoint">
                  not reported
                </span>
              ) : agrees ? (
                <span className="text-up">agrees</span>
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
  const silent = BALANCES.filter((b) => !b.reported)
  const total = reported
    .filter((b) => b.asset === 'USDT')
    .reduce((sum, b) => sum + b.total, 0)

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
        <tr key={`${b.account}:${b.asset}`} className="row-hover border-b border-line/40">
          <td className="px-2 py-1.5">
            <VenueTag venue={b.venue} full />
          </td>
          <td className="px-2 py-1.5 font-semibold text-fg">{b.asset}</td>
          <td className="num px-2 py-1.5 text-right">{num(b.total, b.total < 1 ? 4 : 2)}</td>
          {/* A venue that publishes a total and no free figure leaves a dash.
              Repeating the total there invites committing margin twice. */}
          <td className="num px-2 py-1.5 text-right text-fg">
            {b.available === null ? (
              <span className="text-fg-dim" title="this venue publishes no per-asset free figure">
                —
              </span>
            ) : (
              num(b.available, b.available < 1 ? 4 : 2)
            )}
          </td>
        </tr>
      ))}
      <tr className="border-t border-line bg-panel2/50">
        <td colSpan={2} className="px-2 py-1.5 text-3xs uppercase tracking-[0.08em] text-fg-dim">
          USDT across venues
        </td>
        <td className="num px-2 py-1.5 text-right font-semibold">{num(total)}</td>
        <td className="px-2 py-1.5" />
      </tr>
      {silent.length > 0 && (
        <tr>
          <td colSpan={4} className="px-2 py-1.5 text-3xs text-fg-dim">
            {silent.map((b) => b.venue).join(', ')} report no balance — the total above is a floor,
            not a sum.
          </td>
        </tr>
      )}
    </Grid>
  )
}
