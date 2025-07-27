import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { VenueTag } from '@/components/shared/VenueTag'
import { Icon } from '@/components/shared/Icon'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { useBook, useMarkets, useTickingMarket, type MockListing } from '@/mock/data'

const price = (v: number) =>
  v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5)

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
  const chosenVenue =
    live?.listings.find((l) => l.venue === venue) ?? live?.listings[0] ?? null
  const book = useBook(chosenVenue === null ? null : (chosenVenue.bid + chosenVenue.ask) / 2, 20)

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
                    setVenue(null)
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
        <section className="panel flex max-h-[55%] shrink-0 flex-col">
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
                  <th className="h-7 px-2.5 text-right font-medium">24h vol</th>
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

      <div className="flex w-[268px] shrink-0 flex-col gap-2">
        <OrderTicket
          venue={chosenVenue?.venue ?? null}
          symbol={chosenVenue?.exchangeSymbol ?? null}
          bid={chosenVenue?.bid ?? null}
          ask={chosenVenue?.ask ?? null}
        />
        <section className="panel flex min-h-0 flex-1 flex-col">
          <PanelHeader
            title="Depth"
            right={
              chosenVenue === null ? null : <VenueTag venue={chosenVenue.venue} full />
            }
          />
          <Ladder bids={book.bids} asks={book.asks} />
        </section>
      </div>
    </div>
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
        'cursor-pointer border-b border-line/40 transition-colors duration-75',
        entering && 'enter-fade-up',
        // The selected row is what the ticket and the ladder are pointed at, so
        // it is marked on the left edge rather than by a fill: a whole row of
        // accent colour fights the up/down colours the prices carry.
        selected ? 'bg-accent/8 shadow-[inset_2px_0_0_0_var(--color-accent)]' : 'hover:bg-hover',
      )}
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
      <td className="num px-2.5 py-1.5 text-right text-fg-mid">
        {(listing.volume24h / 1e6).toFixed(1)}M
      </td>
    </tr>
  )
}
