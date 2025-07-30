import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { VenueTag } from '@/components/shared/VenueTag'
import { Icon } from '@/components/shared/Icon'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { useBook, useMarkets, useTickingMarket, type MockListing } from '@/mock/data'

const price = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))

export function MarketsPage() {
  const [query, setQuery] = useState('')
  const [chosen, setChosen] = useState<string | null>(null)
  const [venue, setVenue] = useState<string | null>(null)

  const markets = useMarkets(query)
  const selected = useMemo(
    () => markets.find((m) => m.symbol === chosen) ?? markets[0] ?? null,
    [markets, chosen],
  )
  const live = useTickingMarket(selected)
  const entering = useEntranceOnce(live !== null, live?.symbol)

  // The row you clicked is what the ticket writes against and what the ladder
  // shows. Falling back to the first listing means the right column is never
  // empty on a market you just opened.
  const chosenVenue = live?.listings.find((l) => l.venue === venue) ?? live?.listings[0] ?? null
  const book = useBook(chosenVenue === null ? null : (chosenVenue.bid + chosenVenue.ask) / 2, 20)

  return (
    <div className="flex h-full min-h-0 gap-3">
      <MarketList
        markets={markets}
        selected={selected?.symbol ?? null}
        query={query}
        entering={entering}
        onQuery={(q) => {
          setQuery(q)
          setChosen(null)
        }}
        onPick={(symbol) => {
          setChosen(symbol)
          setVenue(null)
        }}
      />

      <div className="flex min-w-0 flex-1 flex-col gap-3">
        <section className="panel flex max-h-[56%] shrink-0 flex-col">
          <MarketHeader symbol={live?.symbol ?? '—'} venues={live?.listings.length ?? 0} />
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-collapse text-sm whitespace-nowrap">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                  <th className="col-head px-3 text-left font-medium">Venue</th>
                  <th className="col-head px-3 text-left font-medium">Symbol</th>
                  <th className="col-head px-3 text-right font-medium">Bid</th>
                  <th className="col-head px-3 text-right font-medium">Ask</th>
                  <th className="col-head px-3 text-right font-medium">Spread</th>
                  <th className="col-head px-3 text-right font-medium">Funding</th>
                  <th className="col-head px-3 text-right font-medium">24h volume</th>
                </tr>
              </thead>
              <tbody>
                {(live?.listings ?? []).map((l, i) => (
                  <VenueRow
                    key={l.venue}
                    listing={l}
                    selected={chosenVenue?.venue === l.venue}
                    onSelect={() => setVenue(l.venue)}
                    style={entering ? enterDelay(i) : undefined}
                    entering={entering}
                  />
                ))}
              </tbody>
            </table>
          </div>
        </section>

        {/* The board is as tall as the venues that quote the market; the log
            takes whatever is left, which is how a log is meant to sit. */}
        <div className="flex min-h-0 flex-1 flex-col">
          <Blotter />
        </div>
      </div>

      <div className="flex w-[264px] shrink-0 flex-col gap-3">
        <OrderTicket
          listings={live?.listings ?? []}
          venue={chosenVenue?.venue ?? null}
          onVenue={setVenue}
        />
        <section className="panel flex min-h-0 flex-1 flex-col pb-1">
          <PanelHeader
            title="Order book"
            right={chosenVenue === null ? null : <VenueTag venue={chosenVenue.venue} full />}
          />
          <Ladder bids={book.bids} asks={book.asks} />
        </section>
      </div>
    </div>
  )
}

/** The active market, stated once and compactly.
 *
 *  Three facts on two lines rather than five chips on one: what the pair is,
 *  what kind of contract it is, and how many venues are answering. */
function MarketHeader({ symbol, venues }: { symbol: string; venues: number }) {
  return (
    <div className="flex h-14 shrink-0 items-center justify-between px-3">
      <div className="min-w-0">
        <div className="flex items-baseline gap-2">
          <h1 className="text-lg font-semibold tracking-[-0.015em] text-fg">{symbol}</h1>
          <span className="text-2xs font-medium text-fg-dim">PERP</span>
        </div>
        <div className="mt-0.5 flex items-center gap-1.5 text-2xs text-fg-dim">
          <span className="num">{venues}</span>
          <span>venues</span>
          <span className="text-line-strong">·</span>
          <span className="size-[5px] rounded-full bg-up" />
          <span>live</span>
        </div>
      </div>
    </div>
  )
}

function MarketList({
  markets,
  selected,
  query,
  entering,
  onQuery,
  onPick,
}: {
  markets: Array<{ symbol: string; listings: MockListing[] }>
  selected: string | null
  query: string
  entering: boolean
  onQuery: (q: string) => void
  onPick: (symbol: string) => void
}) {
  return (
    <section className="panel flex w-[236px] shrink-0 flex-col">
      <PanelHeader
        title="Markets"
        right={<span className="meta num">{markets.length}</span>}
      />
      <div className="px-3 pb-2">
        <div className="relative">
          <Icon.Search
            className="pointer-events-none absolute top-1/2 left-2.5 size-3.5 -translate-y-1/2 text-fg-dim"
            strokeWidth={1.75}
          />
          <input
            value={query}
            onChange={(e) => onQuery(e.target.value)}
            placeholder="Filter pairs"
            className={cn(
              'h-8 w-full rounded-md bg-raised pr-2.5 pl-8 text-xs text-fg outline-none',
              'transition-shadow duration-150 placeholder:text-fg-dim',
              // Focus is marked at 55% of the accent. A full-strength ring on a
              // field that is focused for most of a session makes the filter
              // the brightest thing on a screen of market data.
              'focus:shadow-[inset_0_0_0_1px_color-mix(in_srgb,var(--color-accent)_55%,transparent)]',
            )}
          />
        </div>
      </div>

      <ul className="min-h-0 flex-1 overflow-auto px-2 pb-2">
        {markets.map((m, i) => {
          const active = m.symbol === selected
          return (
            <li key={m.symbol}>
              <button
                type="button"
                onClick={() => onPick(m.symbol)}
                style={entering ? enterDelay(i) : undefined}
                className={cn(
                  'relative flex h-8 w-full items-center justify-between rounded-md px-2.5',
                  'transition-colors duration-150',
                  entering && 'enter-fade-up',
                  // Selected is a raised surface plus a two-pixel accent edge,
                  // not a bright block: the list is scanned continuously and a
                  // filled slab in it pulls the eye off the board.
                  active
                    ? 'bg-active text-fg before:absolute before:inset-y-1.5 before:left-0 before:w-[2px] before:rounded-full before:bg-accent'
                    : 'text-fg-mid hover:bg-hover hover:text-fg',
                )}
              >
                <span className={cn('truncate text-sm', active && 'font-medium')}>{m.symbol}</span>
                {/* The count is why this view exists: a market four venues
                    quote is worth opening, a market one venue lists is not. */}
                <span className="num shrink-0 text-2xs text-fg-dim">{m.listings.length}</span>
              </button>
            </li>
          )
        })}
        {markets.length === 0 && (
          <EmptyState
            title="Nothing matches"
            hint="The catalogue is what the attached gateways loaded."
          />
        )}
      </ul>
    </section>
  )
}

function VenueRow({
  listing,
  selected,
  onSelect,
  style,
  entering,
}: {
  listing: MockListing
  selected: boolean
  onSelect: () => void
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
      onClick={onSelect}
      className={cn(
        'row cursor-pointer',
        entering && 'enter-fade-up',
        // The selected row is what the ticket and the ladder are pointed at, so
        // it is marked on the left edge rather than by a fill: a whole row of
        // accent colour fights the up/down colours the prices carry.
        selected && 'bg-active shadow-[inset_2px_0_0_0_var(--color-accent)] hover:bg-active',
      )}
    >
      <td className="px-3">
        <VenueTag venue={listing.venue} full />
      </td>
      <td className="px-3 font-mono text-xs text-fg-mid">{listing.exchangeSymbol}</td>
      <td className="px-3 text-right">
        <FlashPrice value={listing.bid} format={price} className="text-up" />
      </td>
      <td className="px-3 text-right">
        <FlashPrice value={listing.ask} format={price} className="text-down" />
      </td>
      <td className="num px-3 text-right text-fg-mid">
        {spread.toFixed(1)}
        <span className="ml-1 text-2xs text-fg-dim">bp</span>
      </td>
      <td className="num px-3 text-right">
        <span className={listing.fundingRate >= 0 ? 'text-up' : 'text-down'}>
          {(listing.fundingRate * 100).toFixed(4)}%
        </span>
        {/* The interval travels with the rate. okx funds every four hours where
            the others fund every eight, and identical numbers side by side
            without it compare two different things. */}
        <span className="ml-1 text-2xs text-fg-dim">/{listing.fundingIntervalHours}h</span>
      </td>
      <td className="num px-3 text-right text-fg">
        {(listing.volume24h / 1e6).toFixed(1)}
        <span className="ml-1 text-2xs text-fg-dim">M</span>
      </td>
    </tr>
  )
}
