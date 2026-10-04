import { memo, useMemo, useState, type ReactNode } from 'react'
import { api, type Fill, type Listing, type Order, type Position } from '@/api/client'
import { EmptyState } from '@/components/shared/EmptyState'
import { Silent } from '@/components/shared/Silent'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { cn, dateTime, fixed, nanosToMillis, num, signed } from '@/lib/format'
import { useQuery, type Query } from '@/lib/query'

type Tab = 'positions' | 'orders' | 'trades'

const k_tabs: Tab[] = ['positions', 'orders', 'trades']

const k_refresh_ms = 5_000

const k_catalogue_refresh_ms = 60_000

const k_fills_shown = '100'

/** shared lead widths keep common columns still across tabs; one column stays unsized */
const k_lead = { id: 92, venue: 128, symbol: 112 }

function Cols({ rest }: { rest: Array<number | null> }) {
  return (
    <colgroup>
      <col style={{ width: k_lead.id }} />
      <col style={{ width: k_lead.venue }} />
      <col style={{ width: k_lead.symbol }} />
      {rest.map((w, i) => (
        <col key={i} style={w === null ? undefined : { width: w }} />
      ))}
    </colgroup>
  )
}

type Catalogue = Map<string, Listing>

const useCatalogue = (): Catalogue => {
  const markets = useQuery(() => api.markets(), [], k_catalogue_refresh_ms)
  return useMemo(() => {
    const map: Catalogue = new Map()
    if (markets.state !== 'ready') return map
    for (const m of markets.data) {
      for (const l of m.listings) map.set(l.instrument_id, l)
    }
    return map
  }, [markets])
}

/** a failed query shows the server's own message, such as the 503 without a dsn */
export function Outcome<T>({ q, empty, children }: {
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

/** no count until the query answers */
const count = (q: Query<{ items: unknown[] } | unknown[]>): string =>
  q.state !== 'ready' ? '' : String(Array.isArray(q.data) ? q.data.length : q.data.items.length)

/** memo: it reads nothing from the market stream, so a tick must not redraw it */
export const Blotter = memo(function Blotter() {
  const [tab, setTab] = useState<Tab>('positions')
  const catalogue = useCatalogue()

  // all queries run so every tab shows its count
  const orders = useQuery(() => api.openOrders(), [], k_refresh_ms)
  const fills = useQuery(() => api.fills(k_fills_shown), [], k_refresh_ms)
  const positions = useQuery(() => api.positions(), [], k_refresh_ms)

  const counts: Record<Tab, string> = {
    orders: count(orders),
    trades: count(fills),
    positions: count(positions),
  }

  return (
    <section className="panel flex min-h-0 flex-1 flex-col">
      <div className="flex h-8 shrink-0 items-center gap-5 px-3">
        {k_tabs.map((t) => (
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
      </div>
    </section>
  )
})

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

export const Th = ({ children, right }: { children: ReactNode; right?: boolean }) => (
  <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left')}>{children}</th>
)
export const Id = ({ children }: { children: string | number }) => (
  <td className="num truncate px-3 text-xs text-fg-dim" title={String(children)}>
    {children}
  </td>
)
export const N = ({ children, className }: { children: ReactNode; className?: string }) => (
  <td className={cn('num px-3 text-right text-fg', className)}>{children}</td>
)

export function When({ nanos }: { nanos: string | undefined }) {
  if (nanos === undefined || nanos === '' || nanos === '0') return <Silent />
  const millis = nanosToMillis(nanos)
  return (
    <span title={nanos}>{Number.isNaN(millis) ? <Silent /> : dateTime(millis)}</span>
  )
}

function Cell({ listing, id }: { listing: Listing | undefined; id: string }) {
  return (
    <>
      <td className="truncate px-3">
        {listing === undefined ? (
          <Silent />
        ) : (
          <Venue venue={listing.venue} market={listing.market === 'spot' ? 'spot' : 'perp'} />
        )}
      </td>
      {listing === undefined ? (
        <td
          className="num truncate px-3 text-2xs text-fg-dim"
          title="no attached gateway lists this instrument — the packed id is all there is"
        >
          {id}
        </td>
      ) : (
        <td className="truncate px-3 text-fg" title={listing.exchange_symbol}>
          {listing.exchange_symbol}
        </td>
      )}
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
            <Cell listing={catalogue.get(o.instrument_id)} id={o.instrument_id} />
            <td className="px-3">
              <StatusBadge status={o.side} />
            </td>
            <td className="px-3 text-fg-mid capitalize">{o.order_type}</td>
            <N>{o.price === undefined || o.price === null ? <Silent /> : o.price}</N>
            {/* filled is the drop in the venue's remaining quantity */}
            <N>
              {qty === null || leaves === null ? (
                <Silent />
              ) : (
                <>
                  <span className={qty - leaves > 0 ? 'text-amber' : 'text-fg-dim'}>
                    {fixed(qty - leaves, 4)}
                  </span>
                  <span className="text-fg-dim"> / {fixed(qty, 4)}</span>
                </>
              )}
            </N>
            <td className="px-3">
              <StatusBadge status={o.status} />
            </td>
            <td className="num px-3 text-right text-xs text-fg-mid">
              <When nanos={o.ts_created} />
            </td>
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
          <Cell listing={catalogue.get(t.instrument_id)} id={t.instrument_id} />
          <td className="px-3">
            <StatusBadge status={t.side} />
          </td>
          <N>{t.price === '' ? <Silent /> : t.price}</N>
          <N>{t.qty === '' ? <Silent /> : t.qty}</N>
          {/* a negative fee is a maker rebate and stays negative */}
          <td className="num px-3 text-right text-fg-mid">
            {t.fee === undefined || t.fee === null ? <Silent /> : t.fee}
          </td>
          <td className="px-3 text-fg-mid capitalize">{t.liquidity ?? 'unknown'}</td>
          <td className="num px-3 text-right text-xs text-fg-mid">
            <When nanos={t.ts_event} />
          </td>
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
            <Cell listing={catalogue.get(p.instrument_id)} id={p.instrument_id} />
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
            {/* null means the venue has not answered; zero means it reported zero */}
            <td className="num px-3 text-right text-fg-mid">
              {p.venue_qty === undefined || p.venue_qty === null ? <Silent /> : p.venue_qty}
            </td>
            <td className="num px-3 text-right text-xs text-fg-mid">
              <When nanos={p.ts_updated} />
            </td>
          </tr>
        )
      })}
    </Grid>
  )
}
