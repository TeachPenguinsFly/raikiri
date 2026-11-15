import { useMemo, useState } from 'react'
import { cn, ExchangeIcon, venueLabel } from '@raikiri/ui'
import { Empty, Flash, Glyph, n, Problem, Seg, Skeleton } from '@raikiri/ui/v3'
import { api } from '../api/client'
import { useQuotes } from '../api/stream'
import { bp, crossOf, px, qty, spreadBp, touchOf, useQuery, useRanked, venueColor, type Kind } from '../lib/market'
import { useTerminal } from './state'

const k_watch = 40

/** the busiest markets with their touch on the first venue, or what the search finds among all of them */
export function WatchlistWidget() {
  const { catalog, pair, pick } = useTerminal()
  const cat = catalog.state === 'ready' ? catalog.data : undefined
  const ranked = useRanked(cat)
  const [q, setQ] = useState('')
  const [kind, setKind] = useState<Kind>(pair?.market ?? 'perp')
  const rows = useMemo(() => {
    const term = q.trim().toUpperCase()
    const all = (ranked.length > 0 ? ranked.map((r) => r.pair) : (cat?.pairs ?? [])).filter((p) => p.market === kind)
    return (term === '' ? all : all.filter((p) => p.base.includes(term))).slice(0, k_watch)
  }, [ranked, cat, q, kind])
  const quotes = useQuotes(useMemo(() => rows.flatMap((p) => (p.legs[0] === undefined ? [] : [p.legs[0].id])), [rows]))

  return (
    <div className="flex h-full min-h-0 flex-col">
      <label className="mx-2 mt-2 mb-1.5 flex h-7 shrink-0 items-center gap-2 rounded-md bg-canvas/60 px-2 text-fg-dim shadow-[0_0_0_1px_var(--color-line)] focus-within:shadow-[0_0_0_1px_rgb(var(--v3-glow)/0.55)]">
        <Glyph name="search" className="size-3" />
        <input
          value={q}
          onChange={(e) => setQ(e.target.value)}
          placeholder="Search"
          aria-label="Search markets"
          className="min-w-0 flex-1 bg-transparent text-xs text-fg outline-none placeholder:text-fg-dim"
        />
      </label>
      <div className="mx-2 mb-1.5">
        <Seg
          size="xs"
          value={kind}
          onChange={setKind}
          options={[
            ['perp', 'Perp'],
            ['spot', 'Spot'],
          ]}
        />
      </div>
      <div className="col-head grid shrink-0 grid-cols-[1fr_auto_52px] gap-2 px-3 pb-1">
        <span>Market</span>
        <span className="text-right">Mid</span>
        <span className="text-right">Spread</span>
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto [scrollbar-width:thin]">
        {catalog.state === 'loading' &&
          Array.from({ length: 10 }, (_, i) => <Skeleton key={i} className="mx-2 my-1 h-7" />)}
        {catalog.state === 'failed' && <Problem title={catalog.title} />}
        {rows.map((p) => {
          const leg = p.legs[0]
          const t = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)
          const on = p.key === pair?.key
          return (
            <button
              key={p.key}
              type="button"
              onClick={() => pick({ market: p.key })}
              className={cn(
                'relative grid h-9 w-full cursor-pointer grid-cols-[1fr_auto_52px] items-center gap-2 px-3 text-left text-xs transition-colors',
                on ? 'bg-accent/[0.07]' : 'hover:bg-raised/60',
              )}
            >
              {on && <span className="absolute inset-y-1.5 left-0 w-0.5 rounded-full bg-accent" />}
              <span className="flex min-w-0 items-center gap-1.5">
                <span className="shrink-0 font-medium text-fg">{p.base}</span>
                <span className="flex min-w-0 items-center -space-x-1 overflow-hidden">
                  {p.legs.slice(0, 3).map((l) => (
                    <ExchangeIcon key={l.venue} venue={l.venue} className="size-3 ring-1 ring-panel" />
                  ))}
                  {p.legs.length > 3 && <span className="pl-1.5 text-[10px] text-fg-dim">+{p.legs.length - 3}</span>}
                </span>
              </span>
              <span className="mono text-right text-fg">
                <Flash value={t?.mid}>{px(t?.mid, leg?.places ?? 2)}</Flash>
              </span>
              <span className="mono text-right text-fg-dim">{spreadBp(t)?.toFixed(1) ?? '—'}</span>
            </button>
          )
        })}
      </div>
    </div>
  )
}

/** the market on every venue that lists it: each touch, its depth at the touch, funding, and where they cross */
export function VenuesWidget() {
  const { pair, leg, quotes, funding, pick } = useTerminal()
  const cross = pair === undefined ? undefined : crossOf(pair, quotes)
  if (pair === undefined) return <Empty title="Pick a market" />
  // spot pays no funding: its column goes with it
  const perp = pair.market === 'perp'
  const cols = perp
    ? 'grid-cols-[minmax(84px,1.1fr)_1fr_1fr_52px_64px_48px]'
    : 'grid-cols-[minmax(84px,1.1fr)_1fr_1fr_52px_48px]'
  const now = Date.now()
  const bestBid = Math.max(...pair.legs.map((l) => touchOf(quotes.get(l.id), l)?.bid ?? -Infinity))
  const bestAsk = Math.min(...pair.legs.map((l) => touchOf(quotes.get(l.id), l)?.ask ?? Infinity))
  return (
    <div className="flex h-full min-h-0 flex-col">
      {cross !== undefined && (
        <div className="flex shrink-0 items-center gap-3 border-b border-line px-3 py-2 text-xs">
          <span className="flex items-center gap-1">
            <span className="text-up">Buy</span>
            <ExchangeIcon venue={cross.buy.leg.venue} className="size-3" />
          </span>
          <Glyph name="arrow" className="size-3 text-fg-dim" />
          <span className="flex items-center gap-1">
            <span className="text-down">Sell</span>
            <ExchangeIcon venue={cross.sell.leg.venue} className="size-3" />
          </span>
          <span className={cn('mono ml-auto font-semibold', cross.bp > 0 ? 'text-up' : 'text-fg-mid')}>
            {cross.bp > 0 ? '+' : ''}
            {bp(cross.bp, 2)}
          </span>
        </div>
      )}
      <div className={cn('col-head grid shrink-0 gap-2 px-3 pt-2 pb-1', cols)}>
        <span>Venue</span>
        <span className="text-right">Bid</span>
        <span className="text-right">Ask</span>
        <span className="text-right">Spread</span>
        {perp && <span className="text-right">Funding</span>}
        <span className="text-right">Age</span>
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto [scrollbar-width:thin]">
        {pair.legs.map((l) => {
          const t = touchOf(quotes.get(l.id), l)
          const f = n(funding.get(l.id)?.rate)
          const on = l.venue === leg?.venue
          return (
            <button
              key={l.venue}
              type="button"
              onClick={() => pick({ venue: l.venue })}
              className={cn(
                'grid h-11 w-full cursor-pointer items-center gap-2 px-3 text-left text-xs transition-colors',
                cols,
                on ? 'bg-raised' : 'hover:bg-raised/50',
              )}
            >
              <span className="flex min-w-0 items-center gap-1.5">
                <span className="h-4 w-0.5 rounded-full" style={{ background: venueColor(l.venue) }} />
                <ExchangeIcon venue={l.venue} className="size-3.5" />
                <span className="truncate text-fg">{venueLabel(l.venue)}</span>
              </span>
              <span className="flex flex-col items-end">
                <span className={cn('mono', t?.bid === bestBid ? 'font-semibold text-up' : 'text-up/80')}>
                  <Flash value={t?.bid}>{px(t?.bid, l.places)}</Flash>
                </span>
                <span className="mono text-[10px] text-fg-dim">{qty(t?.bidSize)}</span>
              </span>
              <span className="flex flex-col items-end">
                <span className={cn('mono', t?.ask === bestAsk ? 'font-semibold text-down' : 'text-down/80')}>
                  <Flash value={t?.ask}>{px(t?.ask, l.places)}</Flash>
                </span>
                <span className="mono text-[10px] text-fg-dim">{qty(t?.askSize)}</span>
              </span>
              <span className="mono text-right text-fg-mid">{spreadBp(t)?.toFixed(2) ?? '—'}</span>
              {perp && (
                <span
                  className={cn(
                    'mono text-right',
                    f === undefined ? 'text-fg-dim' : f >= 0 ? 'text-fg-mid' : 'text-down',
                  )}
                >
                  {f === undefined ? '—' : `${(f * 100).toFixed(4)}%`}
                </span>
              )}
              <span className="mono text-right text-fg-dim">
                {t === undefined || !(t.ts > 0) ? '—' : `${Math.max(0, Math.round((now - t.ts) / 100) / 10)}s`}
              </span>
            </button>
          )
        })}
      </div>
    </div>
  )
}

export function PositionsWidget() {
  const [tab, setTab] = useState<'positions' | 'orders'>('positions')
  const { catalog } = useTerminal()
  const health = useQuery('health', () => api.health(), 5_000)
  // an account's books live in the database; without one there is nothing to ask for
  const db = health.state === 'ready' && health.data.database.reachable
  const positions = useQuery(`positions:${db}`, () => (db ? api.positions() : Promise.resolve(null)), 10_000)
  const orders = useQuery(`orders:${db}`, () => (db ? api.orders() : Promise.resolve(null)), 10_000)
  const held = positions.state === 'ready' ? positions.data : undefined
  const resting = orders.state === 'ready' ? (orders.data?.items ?? null) : undefined
  const cat = catalog.state === 'ready' ? catalog.data : undefined
  const nameOf = (id: string) => {
    const hit = cat?.byId.get(id)
    return hit === undefined ? id : `${hit.pair.base} · ${venueLabel(hit.leg.venue)}`
  }
  const head = 'col-head grid grid-cols-[1.4fr_0.6fr_1fr_1fr] gap-2 px-3 pb-1'
  const row = 'grid h-8 grid-cols-[1.4fr_0.6fr_1fr_1fr] items-center gap-2 px-3 text-xs'
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 items-center px-2 py-1.5">
        <Seg
          size="xs"
          value={tab}
          onChange={setTab}
          options={[
            ['positions', 'Positions'],
            ['orders', 'Open orders'],
          ]}
        />
      </div>
      {tab === 'positions' ? (
        positions.state === 'failed' || held === null ? (
          <Empty title="No account connected" />
        ) : held === undefined ? null : held.length === 0 ? (
          <Empty title="No open positions" />
        ) : (
          <div className="min-h-0 flex-1 overflow-y-auto">
            <div className={head}>
              <span>Market</span>
              <span>Side</span>
              <span className="text-right">Size</span>
              <span className="text-right">Entry</span>
            </div>
            {held.map((p) => {
              const size = n(p.qty) ?? 0
              return (
                <div key={`${p.instrument_id}:${p.strategy_id}`} className={row}>
                  <span className="truncate text-fg">{nameOf(p.instrument_id)}</span>
                  <span className={size >= 0 ? 'text-up' : 'text-down'}>{size >= 0 ? 'Long' : 'Short'}</span>
                  <span className="mono text-right text-fg">{qty(Math.abs(size))}</span>
                  <span className="mono text-right text-fg-mid">{p.avg_entry_price ?? '—'}</span>
                </div>
              )
            })}
          </div>
        )
      ) : orders.state === 'failed' || resting === null ? (
        <Empty title="No account connected" />
      ) : resting === undefined ? null : resting.length === 0 ? (
        <Empty title="No open orders" />
      ) : (
        <div className="min-h-0 flex-1 overflow-y-auto">
          <div className={head}>
            <span>Market</span>
            <span>Side</span>
            <span className="text-right">Size</span>
            <span className="text-right">Price</span>
          </div>
          {resting.map((o) => (
            <div key={o.client_order_id} className={row}>
              <span className="truncate text-fg">{nameOf(o.instrument_id)}</span>
              <span className={o.side === 'buy' ? 'text-up' : 'text-down'}>{o.side}</span>
              <span className="mono text-right text-fg">{o.leaves ?? o.qty}</span>
              <span className="mono text-right text-fg-mid">{o.price ?? 'market'}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
