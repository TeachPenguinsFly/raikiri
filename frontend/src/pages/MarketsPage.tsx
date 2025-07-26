import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { VenueTag } from '@/components/shared/VenueTag'
import { Icon } from '@/components/shared/Icon'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { useBook, useMarkets, useTickingMarket, type MockListing } from '@/mock/data'

const price = (v: number) =>
  v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5)

export function MarketsPage() {
  const [query, setQuery] = useState('')
  const [chosen, setChosen] = useState<string | null>(null)
  const [openBook, setOpenBook] = useState<string | null>(null)

  const markets = useMarkets(query)
  const selected = useMemo(
    () => markets.find((m) => m.symbol === chosen) ?? markets[0] ?? null,
    [markets, chosen],
  )
  const live = useTickingMarket(selected)
  const entering = useEntranceOnce(live !== null, live?.symbol)

  const bookOf = live?.listings.find((l) => l.venue === openBook) ?? null
  const book = useBook(bookOf === null ? null : (bookOf.bid + bookOf.ask) / 2)

  return (
    <div className="flex h-full min-h-0 gap-2 p-2 pt-0">
      <section className="panel flex w-64 shrink-0 flex-col">
        <PanelHeader
          title="Markets"
          right={<span className="micro-label num">{markets.length}</span>}
        />
        <div className="p-2">
          <div className="relative">
            <Icon.Search className="pointer-events-none absolute left-2.5 top-2 size-3.5 text-fg-dim" />
            <input
              value={query}
              onChange={(e) => {
                setQuery(e.target.value)
                setChosen(null)
              }}
              placeholder="BTC, ETH, SOL…"
              autoFocus
              className="h-7 w-full rounded-lg border border-line bg-canvas/60 pl-8 pr-2 text-2xs text-fg outline-none transition-colors placeholder:text-fg-dim focus:border-accent/70"
            />
          </div>
        </div>
        <ul className="min-h-0 flex-1 overflow-auto px-1.5 pb-2">
          {markets.map((m, i) => {
            const active = m.symbol === selected?.symbol
            return (
              <li key={m.symbol}>
                <button
                  type="button"
                  onClick={() => {
                    setChosen(m.symbol)
                    setOpenBook(null)
                  }}
                  style={entering ? enterDelay(i) : undefined}
                  className={cn(
                    'flex h-8 w-full items-center justify-between rounded-lg px-2 text-2xs transition-colors',
                    entering && 'enter-fade-up',
                    active ? 'bg-white/8 font-semibold text-fg' : 'text-fg-mid hover:bg-white/4',
                  )}
                >
                  <span className="truncate">{m.symbol}</span>
                  {/* The count is why this view exists: a market four venues
                      quote is worth opening, a market one venue lists is not. */}
                  <span className="num shrink-0 text-3xs text-fg-dim">{m.listings.length}</span>
                </button>
              </li>
            )
          })}
          {markets.length === 0 && (
            <EmptyState title="Nothing matches" hint="The catalogue is what the attached gateways loaded." />
          )}
        </ul>
      </section>

      <div className="flex min-w-0 flex-1 flex-col gap-2">
        <section className="panel flex min-h-0 flex-1 flex-col">
          <PanelHeader
            title={
              <>
                {live?.symbol ?? '—'}
                <StatusBadge status="perp" />
              </>
            }
            right={
              <span className="micro-label">
                {live?.listings.length ?? 0} venues · live
              </span>
            }
          />
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-collapse text-[11.5px]">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="border-b border-line text-3xs uppercase tracking-[0.08em] text-fg-dim">
                  <th className="h-7 px-2.5 text-left font-medium">Venue</th>
                  <th className="h-7 px-2 text-left font-medium">Symbol</th>
                  <th className="h-7 px-2 text-right font-medium">Bid</th>
                  <th className="h-7 px-2 text-right font-medium">Ask</th>
                  <th className="h-7 px-2 text-right font-medium">Spread</th>
                  <th className="h-7 px-2 text-right font-medium">Funding</th>
                  <th className="h-7 px-2 text-right font-medium">24h vol</th>
                  <th className="h-7 px-2.5 text-right font-medium">Book</th>
                </tr>
              </thead>
              <tbody>
                {(live?.listings ?? []).map((l, i) => (
                  <VenueRow
                    key={l.venue}
                    listing={l}
                    open={openBook === l.venue}
                    onToggle={() => setOpenBook(openBook === l.venue ? null : l.venue)}
                    style={entering ? enterDelay(i) : undefined}
                    entering={entering}
                  />
                ))}
              </tbody>
            </table>
          </div>
        </section>

        {bookOf !== null && (
          <section className="panel flex h-[280px] shrink-0 flex-col">
            <PanelHeader
              title={
                <>
                  <VenueTag venue={bookOf.venue} full />
                  <span className="text-fg-dim">{bookOf.exchangeSymbol}</span>
                </>
              }
              right={
                <button
                  type="button"
                  onClick={() => setOpenBook(null)}
                  className="micro-label hover:text-fg"
                >
                  close
                </button>
              }
            />
            <Ladder bids={book.bids} asks={book.asks} />
          </section>
        )}
      </div>
    </div>
  )
}

function VenueRow({
  listing,
  open,
  onToggle,
  style,
  entering,
}: {
  listing: MockListing
  open: boolean
  onToggle: () => void
  style: React.CSSProperties | undefined
  entering: boolean
}) {
  const mid = (listing.bid + listing.ask) / 2
  // Basis points of the mid, not currency: two venues quoting one market at
  // different tick sizes are not comparable in their own units.
  const spread = ((listing.ask - listing.bid) / mid) * 10_000
  return (
    <tr
      style={style}
      className={cn('row-hover border-b border-line/40', entering && 'enter-fade-up')}
    >
      <td className="px-2.5 py-1.5">
        <VenueTag venue={listing.venue} full />
      </td>
      <td className="px-2 py-1.5 font-mono text-3xs text-fg-dim">{listing.exchangeSymbol}</td>
      <td className="px-2 py-1.5 text-right">
        <FlashPrice value={listing.bid} format={price} className="text-up" />
      </td>
      <td className="px-2 py-1.5 text-right">
        <FlashPrice value={listing.ask} format={price} className="text-down" />
      </td>
      <td className="num px-2 py-1.5 text-right text-fg-mid">{spread.toFixed(1)} bp</td>
      <td className="num px-2 py-1.5 text-right">
        <span className={listing.fundingRate >= 0 ? 'text-up' : 'text-down'}>
          {(listing.fundingRate * 100).toFixed(4)}%
        </span>
        {/* The interval travels with the rate. okx funds every four hours where
            the others fund every eight, and identical numbers side by side
            without it compare two different things. */}
        <span className="ml-1 text-3xs text-fg-dim">/{listing.fundingIntervalHours}h</span>
      </td>
      <td className="num px-2 py-1.5 text-right text-fg-mid">
        {(listing.volume24h / 1e6).toFixed(1)}M
      </td>
      <td className="px-2.5 py-1.5 text-right">
        <button
          type="button"
          onClick={onToggle}
          className={cn(
            'rounded-md px-2 py-0.5 text-3xs font-semibold transition-colors',
            open ? 'bg-accent/15 text-accent' : 'text-fg-dim hover:bg-white/6 hover:text-fg-mid',
          )}
        >
          {open ? 'open' : 'depth'}
        </button>
      </td>
    </tr>
  )
}

/** Depth as a ladder with the size drawn behind the row.
 *
 *  The bar is cumulative, not per level: what a taker wants to know is how far
 *  down the book their order reaches, and a chart of individual levels answers
 *  a question nobody asked. */
function Ladder({
  bids,
  asks,
}: {
  bids: { price: number; size: number }[]
  asks: { price: number; size: number }[]
}) {
  const cumulative = (rows: { price: number; size: number }[]) => {
    let running = 0
    return rows.map((r) => ({ ...r, depth: (running += r.size) }))
  }
  const bidRows = cumulative(bids)
  const askRows = cumulative(asks)
  const peak = Math.max(bidRows.at(-1)?.depth ?? 1, askRows.at(-1)?.depth ?? 1)

  const Side = ({
    rows,
    tone,
    align,
  }: {
    rows: { price: number; size: number; depth: number }[]
    tone: 'up' | 'down'
    align: 'left' | 'right'
  }) => (
    <div className="min-w-0 flex-1">
      {rows.map((r) => (
        <div key={r.price} className="relative flex h-[18px] items-center px-2 text-[11px]">
          <span
            className={cn('absolute inset-y-px', tone === 'up' ? 'bg-up/12' : 'bg-down/12')}
            style={{ width: `${(r.depth / peak) * 100}%`, [align]: 0 }}
          />
          <span
            className={cn(
              'num relative z-10 flex w-full',
              align === 'left' ? 'flex-row-reverse' : 'flex-row',
              'justify-between',
            )}
          >
            <span className="text-fg-dim">{r.size.toFixed(3)}</span>
            <span className={tone === 'up' ? 'text-up' : 'text-down'}>{price(r.price)}</span>
          </span>
        </div>
      ))}
    </div>
  )

  return (
    <div className="flex min-h-0 flex-1 overflow-auto py-1">
      <Side rows={bidRows} tone="up" align="right" />
      <div className="w-px shrink-0 bg-line" />
      <Side rows={askRows} tone="down" align="left" />
    </div>
  )
}
