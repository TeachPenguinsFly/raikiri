import { useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { ExchangeIcon } from '@/components/shared/ExchangeIcon'
import { Icon } from '@/components/shared/Icon'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import {
  useBook,
  useMarkets,
  useTickingMarket,
  type MockListing,
  type MockMarket,
} from '@/mock/data'

const price = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))

/** The best bid across the venues quoting a market — the one figure that
 *  answers "what is this trading at" without picking a venue for the reader. */
const best = (m: MockMarket): number | null =>
  m.listings.length === 0 ? null : Math.max(...m.listings.map((l) => l.bid))

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
  const book = useBook(chosenVenue === null ? null : (chosenVenue.bid + chosenVenue.ask) / 2, 12)

  return (
    <div className="flex h-full min-h-0 gap-2">
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

      <div className="flex min-w-0 flex-1 flex-col gap-2">
        <section className="panel flex max-h-[52%] shrink-0 flex-col">
          <MarketHeader symbol={live?.symbol ?? '—'} venues={live?.listings.length ?? 0} />
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-collapse text-sm whitespace-nowrap">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                  <th className="col-head px-3 text-left">Venue</th>
                  <th className="col-head px-3 text-left">Symbol</th>
                  <th className="col-head px-3 text-right">Bid</th>
                  <th className="col-head px-3 text-right">Ask</th>
                  <th className="col-head px-3 text-right">Spread bp</th>
                  <th className="col-head px-3 text-right">Funding</th>
                  <th className="col-head px-3 text-right">Every</th>
                  <th className="col-head px-3 text-right">24h vol</th>
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

        {/* The board grows with the venues quoting the market — it is what the
            screen is for. The log is a fixed strip: it holds what you did, and
            what you did does not expand to fill a window. */}
        <div className="flex min-h-0 flex-1 flex-col">
          <Blotter />
        </div>
      </div>

      <div className="flex w-64 shrink-0 flex-col gap-2">
        <OrderTicket
          listings={live?.listings ?? []}
          venue={chosenVenue?.venue ?? null}
          onVenue={setVenue}
        />
        <section className="panel flex min-h-0 flex-1 flex-col">
          <PanelHeader
            title="Order book"
            right={
              chosenVenue === null ? null : (
                <span className="meta">{chosenVenue.venue}</span>
              )
            }
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
    <div className="flex h-9 shrink-0 items-center gap-2.5 px-3">
      <span className="text-sm font-semibold tracking-[-0.01em] text-fg">{symbol}</span>
      <span className="text-2xs text-fg-dim">PERP</span>
      <span className="meta ml-auto">{venues} venues</span>
      <span className="size-[5px] rounded-full bg-up" />
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
  markets: MockMarket[]
  selected: string | null
  query: string
  entering: boolean
  onQuery: (q: string) => void
  onPick: (symbol: string) => void
}) {
  return (
    <section className="panel flex w-68 shrink-0 flex-col">
      <PanelHeader
        title="Markets"
        right={<span className="meta num">{markets.length}</span>}
      />
      <div className="px-3 pb-2">
        <div className="relative">
          <Icon.Search
            className="pointer-events-none absolute top-1/2 left-2 size-3 -translate-y-1/2 text-fg-dim"
            strokeWidth={1.75}
          />
          <input
            value={query}
            onChange={(e) => onQuery(e.target.value)}
            placeholder="Filter pairs"
            className={cn(
              'h-7 w-full rounded-sm bg-raised pr-2.5 pl-7 text-xs text-fg outline-none',
              'transition-shadow duration-150 placeholder:text-fg-dim',
              // Focus is marked at 55% of the accent. A full-strength ring on a
              // field that is focused for most of a session makes the filter
              // the brightest thing on a screen of market data.
              'focus:shadow-[inset_0_0_0_1px_color-mix(in_srgb,var(--color-accent)_55%,transparent)]',
            )}
          />
        </div>
      </div>

      <ul className="min-h-0 flex-1 overflow-auto px-1.5 pb-2">
        {markets.map((m, i) => {
          const active = m.symbol === selected
          return (
            <li key={m.symbol}>
              <button
                type="button"
                onClick={() => onPick(m.symbol)}
                style={entering ? enterDelay(i) : undefined}
                className={cn(
                  'relative flex h-7 w-full items-center justify-between overflow-hidden rounded-sm px-2',
                  'transition-colors duration-150',
                  entering && 'enter-fade-up',
                  // Selected is a raised surface plus a two-pixel accent edge,
                  // not a bright block: the list is scanned continuously and a
                  // filled slab in it pulls the eye off the board. The same
                  // edge marks the selected row on the board, so one mark
                  // means one thing across the screen.
                  active
                    ? 'bg-active text-fg before:absolute before:inset-y-0 before:left-0 before:w-[2px] before:bg-accent'
                    : 'text-fg-mid hover:bg-hover hover:text-fg',
                )}
              >
                <span className={cn('truncate text-sm', active && 'font-medium')}>
                  {m.symbol.replace('/USDT', '')}
                  <span className="text-fg-dim">/USDT</span>
                </span>
                <span className="flex shrink-0 items-baseline gap-3">
                  {/* The count is why this view exists: a market four venues
                      quote is worth opening, a market one venue lists is not.
                      The price is why you would open THIS one. */}
                  <span className="num w-3 text-right text-2xs text-fg-dim">
                    {m.listings.length}
                  </span>
                  <span className="num w-20 text-right text-sm text-fg">
                    {best(m) === null ? '—' : price(best(m) as number)}
                  </span>
                </span>
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
      {/* The mark identifies the venue and the word confirms it. This is the
          one table where the logo earns its place: comparing venues IS the
          view, and a column of names alone is a column you read instead of
          scan. Every other table gets the word only. */}
      <td className="flex h-7 items-center gap-2 px-3">
        <ExchangeIcon venue={listing.venue} className="size-[15px]" />
        <span className="text-fg-mid">{listing.venue}</span>
      </td>
      <td className="px-3 text-xs text-fg-mid">{listing.exchangeSymbol}</td>
      <td className="px-3 text-right">
        <FlashPrice value={listing.bid} format={price} className="text-up" />
      </td>
      <td className="px-3 text-right">
        <FlashPrice value={listing.ask} format={price} className="text-down" />
      </td>
      <td className="num px-3 text-right text-fg-mid">{spread.toFixed(1)}</td>
      <td className="num px-3 text-right">
        {/* The sign is always printed, so a positive rate occupies the same
            width as a negative one and the column's digits line up.
            Colour marks only the negative: paying to be long is the exception
            worth finding, and a column that is mostly green is a column the
            eye stops reading. */}
        <span className={listing.fundingRate < 0 ? 'text-down' : 'text-fg'}>
          {listing.fundingRate >= 0 ? '+' : '−'}
          {(Math.abs(listing.fundingRate) * 100).toFixed(4)}%
        </span>
      </td>
      {/* The interval travels with the rate, in a column of its own. okx funds
          every four hours where the others fund every eight, and two identical
          rates side by side without it compare different things. */}
      <td className="num px-3 text-right text-fg-dim">{listing.fundingIntervalHours}h</td>
      <td className="num px-3 text-right text-fg">{(listing.volume24h / 1e6).toFixed(1)}M</td>
    </tr>
  )
}
