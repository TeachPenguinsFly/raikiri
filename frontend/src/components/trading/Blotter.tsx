import { useState, type ReactNode } from 'react'
import { BalanceTree } from '@/components/BalanceTree'
import { ClosePosition } from '@/components/trading/ClosePosition'
import { EmptyState } from '@/components/shared/EmptyState'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { cn } from '@/lib/utils'
import { BALANCES, ORDERS, POSITIONS, TRADES, type MockPosition } from '@/mock/data'

export type Tab = 'orders' | 'trades' | 'positions' | 'balances'

const k_all: Tab[] = ['positions', 'orders', 'trades', 'balances']

/** THE THREE BLOTTERS ARE READ BY SWITCHING BETWEEN THEM, so the columns they
 *  share must not move when you do.
 *
 *  On the default `auto` table layout every table measures its own content and
 *  arrives at its own widths, so tabbing from Positions to Orders slid the id,
 *  the venue and the symbol sideways — three columns that hold the same thing
 *  in all three tables. Fixed layout plus one shared prefix pins them: the eye
 *  keeps its anchor and only the columns that genuinely differ differ.
 *
 *  Exactly one column per table is left unsized, and it absorbs whatever the
 *  panel has spare. Sizing all of them would make the browser scale every
 *  column to fill the width, which is the same drift by another route. */
const LEAD = { id: 92, venue: 128, symbol: 112 }

function Cols({ rest }: { rest: Array<number | null> }) {
  return (
    <colgroup>
      <col style={{ width: LEAD.id }} />
      <col style={{ width: LEAD.venue }} />
      <col style={{ width: LEAD.symbol }} />
      {rest.map((w, i) => (
        <col key={i} style={w === null ? undefined : { width: w }} />
      ))}
    </colgroup>
  )
}

const num = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })
const signed = (v: number, dp = 2) => `${v >= 0 ? '+' : '−'}${num(Math.abs(v), dp)}`

/** The bottom of the screen, where what you have done lives. */
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
      <div className="flex h-8 shrink-0 items-center gap-5 px-3">
        {tabs.map((t) => (
          <button
            key={t}
            type="button"
            onClick={() => setTab(t)}
            className={cn(
              'relative flex h-8 items-center gap-1.5 text-xs capitalize transition-colors duration-150',
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
        {tab === 'balances' && <BalanceTree />}
      </div>
    </section>
  )
}

function Grid({
  cols,
  head,
  children,
}: {
  cols: Array<number | null>
  head: ReactNode
  children: ReactNode
}) {
  return (
    <table className="w-full min-w-[1080px] table-fixed border-collapse text-sm whitespace-nowrap">
      <Cols rest={cols} />
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">{head}</tr>
      </thead>
      <tbody>{children}</tbody>
    </table>
  )
}

const Th = ({ children, right }: { children: ReactNode; right?: boolean }) => (
  <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left')}>{children}</th>
)
const Id = ({ children }: { children: ReactNode }) => (
  <td className="num px-3 text-xs text-fg-dim">{children}</td>
)
const N = ({ children, className }: { children: ReactNode; className?: string }) => (
  <td className={cn('num px-3 text-right text-fg', className)}>{children}</td>
)

function Orders() {
  if (ORDERS.length === 0) return <EmptyState title="Nothing working" />
  return (
    <Grid
      cols={[64, 80, 112, 116, null, 168]}
      head={
        <>
          <Th>ID</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th>Type</Th>
          <Th right>Price</Th>
          <Th right>Filled</Th>
          <Th>Status</Th>
          <Th right>Time</Th>
        </>
      }
    >
      {ORDERS.map((o) => (
        <tr key={o.id} className="row">
          <Id>{o.id}</Id>
          <td className="px-3">
            <Venue venue={o.venue} market={o.market} />
          </td>
          <td className="px-3 text-fg">{o.symbol}</td>
          <td className="px-3">
            <StatusBadge status={o.side} />
          </td>
          <td className="px-3 text-fg-mid capitalize">{o.type}</td>
          <N>{o.price === null ? '—' : num(o.price, 1)}</N>
          <N>
            <span className={o.filled > 0 ? 'text-amber' : 'text-fg-dim'}>{o.filled}</span>
            <span className="text-fg-dim"> / {o.qty}</span>
          </N>
          <td className="px-3">
            <StatusBadge status={o.status} />
          </td>
          {/* The stamp, not an age. An age has to be recomputed before it can
              be compared with anything else in the journal. */}
          <td className="num px-3 text-right text-xs text-fg-mid">{o.placedAt}</td>
        </tr>
      ))}
    </Grid>
  )
}

function Trades() {
  return (
    <Grid
      cols={[64, 112, 88, 96, 96, null, 168]}
      head={
        <>
          <Th>ID</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th right>Price</Th>
          <Th right>Qty</Th>
          <Th right>Fee</Th>
          <Th right>Funding</Th>
          <Th>Type</Th>
          <Th right>Time</Th>
        </>
      }
    >
      {TRADES.map((t) => (
        <tr key={t.id} className="row">
          <Id>{t.id}</Id>
          <td className="px-3">
            <Venue venue={t.venue} market={t.market} />
          </td>
          <td className="px-3 text-fg">{t.symbol}</td>
          <td className="px-3">
            <StatusBadge status={t.side} />
          </td>
          <N>{num(t.price, 1)}</N>
          <N>{t.qty}</N>
          <td className="num px-3 text-right text-fg-mid">{t.fee.toFixed(4)}</td>
          {/* Funding accrued on the position this fill belongs to. Spot has
              none and shows nothing — a zero would claim it was measured. */}
          <td
            className={cn(
              'num px-3 text-right',
              t.fundingUsd === null ? 'text-fg-dim' : t.fundingUsd >= 0 ? 'text-up' : 'text-down',
            )}
          >
            {t.fundingUsd === null ? '—' : signed(t.fundingUsd)}
          </td>
          <td className="px-3 text-fg-mid capitalize">{t.liquidity}</td>
          <td className="num px-3 text-right text-xs text-fg-mid">{t.at}</td>
        </tr>
      ))}
    </Grid>
  )
}

function Positions() {
  const [closing, setClosing] = useState<MockPosition | null>(null)
  return (
    <>
    <Grid
      cols={[88, 52, 104, 104, 104, 96, 104, null, 120]}
      head={
        <>
          <Th>ID</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th right>Qty</Th>
          <Th right>Lev</Th>
          <Th right>Entry</Th>
          <Th right>Mark</Th>
          <Th right>Liq.</Th>
          <Th right>Funding</Th>
          <Th right>Unrealized</Th>
          <Th>Owner</Th>
          <Th right>Close</Th>
        </>
      }
    >
      {POSITIONS.map((p) => {
        const pnl = (p.mark - p.entry) * p.qty
        return (
          <tr key={p.id} className="row">
            <Id>{p.id}</Id>
            <td className="px-3">
              <Venue venue={p.venue} market={p.market} />
            </td>
            <td className="px-3 text-fg">{p.symbol}</td>
            <N className={p.qty >= 0 ? 'text-up' : 'text-down'}>
              {p.qty > 0 ? '+' : ''}
              {p.qty}
            </N>
            <td className="num px-2 text-right text-fg-mid">{p.leverage}×</td>
            <td className="num px-3 text-right text-fg-mid">{num(p.entry, 1)}</td>
            <N>{num(p.mark, 1)}</N>
            {/* Spot cannot be liquidated, so it shows nothing rather than a
                price that does not exist. */}
            <td className="num px-3 text-right text-amber">
              {p.liqPrice === null ? <span className="text-fg-dim">—</span> : num(p.liqPrice, 1)}
            </td>
            <td
              className={cn(
                'num px-3 text-right',
                p.fundingUsd === null ? 'text-fg-dim' : p.fundingUsd >= 0 ? 'text-up' : 'text-down',
              )}
            >
              {p.fundingUsd === null ? '—' : signed(p.fundingUsd)}
            </td>
            <N className={pnl >= 0 ? 'text-up' : 'text-down'}>{signed(pnl)}</N>
            <td className="px-2 text-xs text-fg-mid">
              {p.strategy ?? <span className="text-fg-dim">manual</span>}
            </td>
            {/* Only a hand-opened position gets close controls. A strategy's
                position is closed by the strategy — a button that unwinds one
                behind its back leaves the strategy holding a leg it thinks it
                still has. */}
            <td className="px-2 text-right">
              {p.strategy === null ? (
                <span className="inline-flex gap-1">
                  <CloseButton onClick={() => setClosing(p)}>Limit</CloseButton>
                  <CloseButton>Market</CloseButton>
                </span>
              ) : null}
            </td>
          </tr>
        )
      })}
    </Grid>
    {closing !== null && (
      <ClosePosition position={closing} onClose={() => setClosing(null)} />
    )}
    </>
  )
}

function CloseButton({ children, onClick }: { children: ReactNode; onClick?: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className="h-5 rounded-sm bg-raised px-1.5 text-2xs text-fg-mid transition-colors duration-150 hover:bg-hover hover:text-fg"
    >
      {children}
    </button>
  )
}
