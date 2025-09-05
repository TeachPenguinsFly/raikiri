import { useMemo, useState, type ReactNode } from 'react'
import { api, type Fill, type Listing, type Order, type Position } from '@/api/client'
import { BalanceTree } from '@/components/BalanceTree'
import { EmptyState } from '@/components/shared/EmptyState'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { num } from '@/lib/live'
import { useQuery, type Query } from '@/lib/query'
import { cn } from '@/lib/utils'

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

const dp = (v: number, places = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: places, maximumFractionDigits: places })
const signed = (v: number, places = 2) => `${v >= 0 ? '+' : '−'}${dp(Math.abs(v), places)}`

/** A figure the venue did not publish. Never a zero: a zero is a measurement. */
const Silent = () => <span className="text-fg-dim">—</span>

/** What the catalogue says about an instrument, keyed by the id every ledger
 *  row carries. Orders, fills and positions name only the id — venue and symbol
 *  live here, and a row whose id the catalogue does not know shows the id
 *  rather than inventing a name for it. */
type Catalogue = Map<string, Listing>

const useCatalogue = (): Catalogue => {
  const markets = useQuery(() => api.markets({}), [], 60_000)
  return useMemo(() => {
    const map: Catalogue = new Map()
    if (markets.state !== 'ready') return map
    for (const m of markets.data) {
      for (const l of m.listings) map.set(l.instrument_id, l)
    }
    return map
  }, [markets])
}

/** Loading, failed and empty are three different screens.
 *
 *  A failed query renders the server's own words: `/v1/orders` and its
 *  neighbours answer 503 with a message explaining that admind was started
 *  without a DSN, and that message is the whole answer to "why is this table
 *  empty" — replacing it with an empty table asserts that nothing happened. */
function Outcome<T>({ q, empty, children }: {
  q: Query<T>
  empty: string
  children: (data: T) => ReactNode
}) {
  if (q.state === 'loading') return <EmptyState title="Asking admind" />
  if (q.state === 'failed') {
    return (
      <div className="flex flex-col gap-1 p-4">
        <span className="text-sm text-down">{q.title}</span>
        {q.detail !== undefined && <span className="text-xs text-fg-mid">{q.detail}</span>}
      </div>
    )
  }
  return <>{Array.isArray(q.data) && q.data.length === 0 ? <EmptyState title={empty} /> : children(q.data)}</>
}

/** The bottom of the screen, where what you have done lives. */
export function Blotter({ tabs = k_all }: { tabs?: Tab[] }) {
  const [tab, setTab] = useState<Tab>(tabs[0] as Tab)
  const catalogue = useCatalogue()

  // All four run, not just the open one: the counts on the tabs are the reason
  // to switch, and a count that only appears after you switch is not one.
  const orders = useQuery(() => api.orders({ open: true }), [], 5_000)
  const fills = useQuery(() => api.fills({ limit: '100' }), [], 5_000)
  const positions = useQuery(() => api.positions(), [], 5_000)
  const balances = useQuery(() => api.balances(), [], 10_000)

  // NO COUNT until the answer arrived. A zero beside a tab whose query failed
  // would read as "nothing here", which is the one thing it does not know.
  const count = (q: Query<{ items: unknown[] } | unknown[]>): string =>
    q.state !== 'ready' ? '' : String(Array.isArray(q.data) ? q.data.length : q.data.items.length)

  const counts: Record<Tab, string> = {
    orders: count(orders),
    trades: count(fills),
    positions: count(positions),
    balances: count(balances),
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
        {tab === 'orders' && (
          <Outcome q={orders} empty="Nothing working">
            {(d) => <Orders rows={d.items} catalogue={catalogue} />}
          </Outcome>
        )}
        {tab === 'trades' && (
          <Outcome q={fills} empty="No fills yet">
            {(d) => <Trades rows={d.items} catalogue={catalogue} />}
          </Outcome>
        )}
        {tab === 'positions' && (
          <Outcome q={positions} empty="Flat everywhere">
            {(d) => <Positions rows={d} catalogue={catalogue} />}
          </Outcome>
        )}
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

const where = (c: Catalogue, id: string) => c.get(id)

function Cell({ listing, id }: { listing: Listing | undefined; id: string }) {
  return (
    <>
      <td className="px-3">
        {listing === undefined ? (
          <Silent />
        ) : (
          <Venue venue={listing.venue} market={listing.market === 'spot' ? 'spot' : 'perp'} />
        )}
      </td>
      {/* The id when the catalogue does not know it: a row whose instrument
          this gateway never loaded is still a real row, and blanking it would
          hide the one case worth looking at. */}
      <td className="px-3 text-fg">{listing?.exchange_symbol ?? id}</td>
    </>
  )
}

function Orders({ rows, catalogue }: { rows: Order[]; catalogue: Catalogue }) {
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
          <Th right>Created</Th>
        </>
      }
    >
      {rows.map((o) => {
        const qty = num(o.qty)
        const leaves = num(o.leaves)
        return (
          <tr key={o.client_order_id} className="row">
            <Id>{o.client_order_id}</Id>
            <Cell listing={where(catalogue, o.instrument_id)} id={o.instrument_id} />
            <td className="px-3">
              <StatusBadge status={o.side} />
            </td>
            <td className="px-3 text-fg-mid capitalize">{o.order_type}</td>
            <N>{o.price === undefined || o.price === null ? <Silent /> : o.price}</N>
            {/* FILLED IS A DIFFERENCE, not a field: what the venue reports is
                what is still working, and the fill is the drop in it. */}
            <N>
              {qty === null || leaves === null ? (
                <Silent />
              ) : (
                <>
                  <span className={qty - leaves > 0 ? 'text-amber' : 'text-fg-dim'}>
                    {dp(qty - leaves, 4)}
                  </span>
                  <span className="text-fg-dim"> / {dp(qty, 4)}</span>
                </>
              )}
            </N>
            <td className="px-3">
              <StatusBadge status={o.status} />
            </td>
            {/* The stamp, not an age. An age has to be recomputed before it can
                be compared with anything else in the journal. */}
            <td className="num px-3 text-right text-xs text-fg-mid">{o.ts_created}</td>
          </tr>
        )
      })}
    </Grid>
  )
}

function Trades({ rows, catalogue }: { rows: Fill[]; catalogue: Catalogue }) {
  return (
    <Grid
      cols={[64, 112, 88, 96, 96, null]}
      head={
        <>
          <Th>Exec</Th>
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
      {rows.map((t) => (
        <tr key={`${t.venue}:${t.exec_id}`} className="row">
          <Id>{t.exec_id}</Id>
          <Cell listing={where(catalogue, t.instrument_id)} id={t.instrument_id} />
          <td className="px-3">
            <StatusBadge status={t.side} />
          </td>
          <N>{t.price}</N>
          <N>{t.qty}</N>
          {/* A NEGATIVE FEE IS A MAKER REBATE and stays negative: folding it
              into a cost would hide the one case a desk is trying to earn. */}
          <td className="num px-3 text-right text-fg-mid">
            {t.fee === undefined || t.fee === null ? <Silent /> : t.fee}
          </td>
          <td className="px-3 text-fg-mid capitalize">{t.liquidity ?? 'unknown'}</td>
          <td className="num px-3 text-right text-xs text-fg-mid">{t.ts_event}</td>
        </tr>
      ))}
    </Grid>
  )
}

function Positions({ rows, catalogue }: { rows: Position[]; catalogue: Catalogue }) {
  return (
    <Grid
      cols={[88, 52, 104, 104, 104, null]}
      head={
        <>
          <Th>Strategy</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th right>Qty</Th>
          <Th right>Entry</Th>
          <Th right>Realized</Th>
          <Th right>Venue says</Th>
          <Th right>Updated</Th>
        </>
      }
    >
      {rows.map((p) => {
        const qty = num(p.qty)
        return (
          <tr key={`${p.strategy_id}:${p.instrument_id}`} className="row">
            <Id>{p.strategy_id}</Id>
            <Cell listing={where(catalogue, p.instrument_id)} id={p.instrument_id} />
            <N className={qty !== null && qty < 0 ? 'text-down' : 'text-up'}>
              {qty === null ? <Silent /> : signed(qty, 4)}
            </N>
            <td className="num px-3 text-right text-fg-mid">
              {p.avg_entry_price === undefined || p.avg_entry_price === null ? (
                <Silent />
              ) : (
                p.avg_entry_price
              )}
            </td>
            <N>{p.realized === undefined || p.realized === null ? <Silent /> : p.realized}</N>
            {/* NULL IS NOT FLAT. A venue that has not answered and a venue
                reporting zero are different facts, and this column exists to
                keep them apart rather than to agree with the one beside it. */}
            <td className="num px-3 text-right text-fg-mid">
              {p.venue_qty === undefined || p.venue_qty === null ? <Silent /> : p.venue_qty}
            </td>
            <td className="num px-3 text-right text-xs text-fg-mid">{p.ts_updated}</td>
          </tr>
        )
      })}
    </Grid>
  )
}
