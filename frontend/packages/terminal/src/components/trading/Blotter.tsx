import { memo, useMemo, useState, type ReactNode } from 'react'
import {
  cn,
  dateTime,
  EmptyState,
  fixed,
  nanosToMillis,
  num,
  signed,
  Silent,
  StatusBadge,
  Venue,
} from '@raikiri/ui'
import {
  api,
  joe,
  type BookRecord,
  type Fill,
  type Listing,
  type Order,
  type ParentOrder,
  type Position,
} from '../../api/client'
import { keepNewest } from '../../lib/latest'
import { useQuery, type Query } from '../../lib/query'

type Tab = 'positions' | 'orders' | 'trades'

const newestOrders = <P extends { items: Order[] }>(held: P, next: P): P => ({
  ...next,
  items: keepNewest(held.items, next.items, (o) => o.client_order_id),
})

const newestPositions = (held: Position[], next: Position[]) =>
  keepNewest(held, next, (p) => `${p.instrument_id}:${p.strategy_id}`)

const k_tabs: Tab[] = ['positions', 'orders', 'trades']

const k_refresh_ms = 5_000

const k_catalogue_refresh_ms = 60_000

const k_fills_shown = '100'

/** shared lead widths keep common columns still across tabs; one column stays unsized */
const k_lead = { id: 150, venue: 112, symbol: 112 }

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

  const [book, setBook] = useState('')
  const wanted = book === '' ? undefined : book

  // all queries run so every tab shows its count
  const orders = useQuery(() => api.openOrders(wanted), [wanted], k_refresh_ms, newestOrders)
  const fills = useQuery(() => api.fills(k_fills_shown, wanted), [wanted], k_refresh_ms)
  const positions = useQuery(() => api.positions(wanted), [wanted], k_refresh_ms, newestPositions)
  const parents = useQuery(() => joe.parents(), [], k_refresh_ms)
  const books = useQuery(() => api.books(), [], k_catalogue_refresh_ms)

  const counts: Record<Tab, string> = {
    orders: count(orders),
    trades: count(fills),
    positions: count(positions),
  }

  return (
    <section data-blotter className="panel flex min-h-0 flex-1 flex-col">
      <div data-drag-handle className="flex h-8 shrink-0 items-center gap-5 px-3">
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
        <select
          aria-label="Book"
          value={book}
          onChange={(e) => setBook(e.target.value)}
          className="ml-auto h-6 max-w-44 rounded bg-raised px-1.5 text-xs text-fg-mid outline-none"
        >
          <option value="">All books</option>
          {books.state === 'ready' &&
            books.data.map((b) => (
              <option key={b.id} value={String(b.id)}>
                {b.name}
              </option>
            ))}
        </select>
      </div>

      <div className="min-h-0 flex-1 overflow-auto">
        {tab === 'orders' && (
          <Outcome q={orders} empty="Nothing working">
            {(d) => (
              <Orders
                rows={d.items}
                parents={parents.state === 'ready' ? parents.data.items.filter((p) => inBook(p, wanted)) : []}
                catalogue={catalogue}
              />
            )}
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
    <table className="w-full min-w-[1150px] table-fixed border-collapse text-sm whitespace-nowrap">
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

const k_kind_colour: Record<BookRecord['kind'], string> = {
  strategy: 'text-fg-mid',
  manual: 'text-fg',
  client: 'text-accent',
  hedge: 'text-amber',
}

/** a number with nothing on record shows as the number: every order has a book, however old */
function BookCell({ id, name, kind }: { id: number | undefined; name: string | undefined; kind: BookRecord['kind'] | undefined }) {
  if (id === undefined) return <td className="px-3"><Silent /></td>
  return (
    <td
      className={cn('truncate px-3 text-xs', kind === undefined ? 'text-fg-mid' : k_kind_colour[kind])}
      title={`${kind ?? 'book'} ${id}`}
    >
      {name ?? `#${id}`}
    </td>
  )
}

/** why the order exists: the reference it answers to, else the parent working it, else a person's or a strategy's own */
type Origin = { book_kind?: BookRecord['kind'] | undefined; parent_id?: string | undefined; external_ref?: string | undefined }

function originText(o: Origin): string {
  if (o.external_ref !== undefined) return o.external_ref
  if (o.parent_id !== undefined) return `parent ${o.parent_id}`
  return o.book_kind === 'manual' ? 'by hand' : ''
}

function OriginCell({ of }: { of: Origin }) {
  const text = originText(of)
  return (
    <td className="num truncate px-3 text-xs text-fg-mid" title={of.parent_id === undefined ? text : `parent ${of.parent_id}`}>
      {text === '' ? <Silent /> : text}
    </td>
  )
}

const inBook = (p: ParentOrder, book: string | undefined): boolean =>
  !isTerminal(p.status) && (book === undefined || String(p.book_id) === book)

const isTerminal = (status: string): boolean => status === 'done' || status === 'cancelled' || status === 'failed'

const k_order_cols = [112, 112, 56, 64, 96, 128, null, 136]

function OrderHead() {
  return (
    <>
      <Th>ID</Th>
      <Th>Venue</Th>
      <Th>Symbol</Th>
      <Th>Book</Th>
      <Th>Origin</Th>
      <Th>Side</Th>
      <Th>Type</Th>
      <Th right>Price</Th>
      <Th right>Filled</Th>
      <Th>Status</Th>
      <Th right>Created</Th>
    </>
  )
}

function OrderRow({ o, catalogue, nested }: { o: Order; catalogue: Catalogue; nested?: boolean }) {
  const qty = num(o.qty)
  const leaves = num(o.leaves)
  return (
    <tr data-child={nested === true ? o.parent_id : undefined} className={cn('row', nested === true && 'bg-raised/40')}>
      <td className={cn('num truncate pr-3 text-xs text-fg-dim', nested === true ? 'pl-7' : 'pl-3')} title={o.client_order_id}>
        {o.client_order_id}
      </td>
      <Cell listing={catalogue.get(o.instrument_id)} id={o.instrument_id} />
      <BookCell id={o.book_id} name={o.book} kind={o.book_kind} />
      <OriginCell of={o} />
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
            <span className={qty - leaves > 0 ? 'text-amber' : 'text-fg-dim'}>{fixed(qty - leaves, 4)}</span>
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
}

/** one parent: joe's own row when it still holds the parent, else what its open children add up to */
type ParentGroup = { id: string; live: ParentOrder | undefined; open: Order[] }

function groupByParent(rows: Order[], parents: ParentOrder[]): { groups: ParentGroup[]; loose: Order[] } {
  const groups = new Map<string, ParentGroup>()
  for (const p of parents) groups.set(p.id, { id: p.id, live: p, open: [] })
  const loose: Order[] = []
  for (const o of rows) {
    if (o.parent_id === undefined) {
      loose.push(o)
      continue
    }
    const group = groups.get(o.parent_id) ?? { id: o.parent_id, live: undefined, open: [] }
    group.open.push(o)
    groups.set(o.parent_id, group)
  }
  return { groups: [...groups.values()], loose }
}

function ParentRow({ group, catalogue }: { group: ParentGroup; catalogue: Catalogue }) {
  const [open, setOpen] = useState(false)
  const { live, open: working } = group
  const first = working[0]
  const qty = num(live?.qty)
  const filled = num(live?.filled)
  const listing = first === undefined ? undefined : catalogue.get(first.instrument_id)
  return (
    <>
      <tr data-parent={group.id} className="row cursor-pointer" onClick={() => setOpen(!open)}>
        <td className="num truncate pl-3 pr-3 text-xs text-fg" title={`parent ${group.id}`}>
          <span aria-hidden className="mr-1 inline-block w-3 text-fg-dim">
            {open ? '▾' : '▸'}
          </span>
          {group.id}
        </td>
        <td className="truncate px-3 text-fg-mid">{live !== undefined ? `${live.child_count} children` : `${working.length} open`}</td>
        <td className="truncate px-3 text-fg">{live?.symbol ?? listing?.exchange_symbol ?? <Silent />}</td>
        <BookCell id={live?.book_id ?? first?.book_id} name={live?.book ?? first?.book} kind={live?.book_kind ?? first?.book_kind} />
        <OriginCell of={{ book_kind: live?.book_kind ?? first?.book_kind, external_ref: live?.external_ref ?? first?.external_ref }} />
        <td className="px-3">{live !== undefined ? <StatusBadge status={live.side} /> : first !== undefined ? <StatusBadge status={first.side} /> : <Silent />}</td>
        <td className="px-3 text-fg-mid uppercase">{live?.algo ?? 'parent'}</td>
        <N>{live?.avg_price ?? <Silent />}</N>
        <N>
          {qty === null || filled === null ? (
            <Silent />
          ) : (
            <>
              <span className={filled > 0 ? 'text-amber' : 'text-fg-dim'}>{fixed(filled, 4)}</span>
              <span className="text-fg-dim"> / {fixed(qty, 4)}</span>
            </>
          )}
        </N>
        <td className="px-3">
          <StatusBadge status={live?.status ?? 'working'} />
        </td>
        <td className="num px-3 text-right text-xs text-fg-mid">
          <When nanos={live?.started ?? live?.created ?? first?.ts_created} />
        </td>
      </tr>
      {open && <Children parent={group.id} catalogue={catalogue} />}
    </>
  )
}

/** every child the journal holds, done ones too; asked only once a parent is open */
function Children({ parent, catalogue }: { parent: string; catalogue: Catalogue }) {
  const children = useQuery(() => api.childOrders(parent), [parent], k_refresh_ms)
  if (children.state !== 'ready' || children.data.items.length === 0) {
    return (
      <tr>
        <td colSpan={11} className="py-1 pl-7 text-xs text-fg-dim">
          {children.state === 'failed' ? children.title : children.state === 'loading' ? 'Asking admind' : 'No children yet'}
        </td>
      </tr>
    )
  }
  return (
    <>
      {children.data.items.map((o) => (
        <OrderRow key={o.client_order_id} o={o} catalogue={catalogue} nested />
      ))}
    </>
  )
}

function Orders({ rows, parents, catalogue }: { rows: Order[]; parents: ParentOrder[]; catalogue: Catalogue }) {
  const { groups, loose } = groupByParent(rows, parents)
  return (
    <Grid cols={k_order_cols} head={<OrderHead />}>
      {groups.map((g) => (
        <ParentRow key={g.id} group={g} catalogue={catalogue} />
      ))}
      {loose.map((o) => (
        <OrderRow key={o.client_order_id} o={o} catalogue={catalogue} />
      ))}
    </Grid>
  )
}

function Trades({ rows, catalogue }: { rows: Fill[]; catalogue: Catalogue }) {
  return (
    <Grid
      cols={[112, 112, 64, 96, 88, 80, 80, null]}
      head={
        <>
          <Th>Exec</Th>
          <Th>Venue</Th>
          <Th>Symbol</Th>
          <Th>Book</Th>
          <Th>Origin</Th>
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
          <BookCell id={t.book_id} name={t.book} kind={t.book_kind} />
          <OriginCell of={t} />
          <td className="px-3">
            <StatusBadge status={t.side} />
          </td>
          <N>{t.price === '' ? <Silent /> : t.price}</N>
          <N>{t.qty === '' ? <Silent /> : t.qty}</N>
          {/* a negative fee is a maker rebate and stays negative */}
          <td className="num px-3 text-right text-fg-mid">
            {t.fee === undefined || t.fee === null ? <Silent /> : t.fee}
          </td>
          <td className={t.liquidity === 'internal' ? 'px-3 font-medium text-accent capitalize' : 'px-3 text-fg-mid capitalize'} data-testid={t.liquidity === 'internal' ? 'fill-internal' : undefined} title={t.liquidity === 'internal' ? 'crossed inside the desk; no venue saw it' : undefined}>{t.liquidity ?? 'unknown'}</td>
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
          <Th>Book</Th>
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
            <BookCell id={p.strategy_id} name={p.book} kind={p.book_kind} />
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
