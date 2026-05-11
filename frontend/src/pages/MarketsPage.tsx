import { useEffect, useMemo, useState } from 'react'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Venue } from '@/components/shared/Venue'
import { Icon } from '@/components/shared/Icon'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { compact } from '@/lib/format'
import { cn } from '@/lib/utils'
import { api, type Market } from '@/api/client'
import type { components } from '@/api/types'
import { num, type LiveListing, type LiveMarket } from '@/lib/live'
import { useQuery } from '@/lib/query'
import { useBook, useFunding, useQuotes, useTicker } from '@/lib/stream'

const price = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))


/** What the board can be ordered by. Funding is the reason the sort exists:
 *  the venue paying the most and the venue charging the most are the two ends
 *  of a carry, and finding them by eye across sixteen rows is the work this
 *  removes. */
// Volume is the QUOTE column, never the base one: ranking sixteen venues by
// coins traded puts an inverse contract on top, because its base is the coin
// the contract is priced in rather than the one it settles.
type MarketType = components['schemas']['MarketType']

const k_market_order: MarketType[] = ['perp', 'spot', 'future', 'option']

const venueCount = (m: Market) => new Set(m.listings.map((l) => l.venue)).size

type SortKey = 'venue' | 'bid' | 'ask' | 'spread' | 'funding' | 'volume'

/** A venue that has not quoted sorts LAST whichever way the column runs, so a
 *  silent venue never displaces a live one at the top of the board. */
const sortValue = (l: LiveListing, key: SortKey): number | string | null => {
  switch (key) {
    case 'venue':
      return l.venue
    case 'bid':
      return l.bid
    case 'ask':
      return l.ask
    case 'spread': {
      if (l.bid === null || l.ask === null) return null
      const mid = (l.bid + l.ask) / 2
      return mid === 0 ? null : ((l.ask - l.bid) / mid) * 10_000
    }
    // Normalised to eight hours before comparing. okx funds every four and
    // hyperliquid every hour; two raw rates side by side compare different
    // things.
    case 'funding':
      return l.fundingRate === null || l.fundingIntervalHours === null ||
             l.fundingIntervalHours === 0
        ? null
        : (l.fundingRate / l.fundingIntervalHours) * 8
    case 'volume':
      return l.volume24h
  }
}

/** The pairs a desk actually watches, kept in the browser.
 *
 *  Per-viewer, not per-desk: which markets one person keeps an eye on is a
 *  convenience, not a position, and putting it on the server would make two
 *  people on one account fight over the list. A read that throws — a private
 *  window, cleared site data — starts empty rather than breaking the screen. */
const k_favorites = 'raikiri.favorites'

function useFavorites() {
  const [favorites, setFavorites] = useState<Set<string>>(() => {
    try {
      return new Set(JSON.parse(localStorage.getItem(k_favorites) ?? '[]') as string[])
    } catch {
      return new Set()
    }
  })
  useEffect(() => {
    try {
      localStorage.setItem(k_favorites, JSON.stringify([...favorites]))
    } catch {
      // A viewer with site data blocked keeps their list for the session only.
    }
  }, [favorites])

  const toggle = (symbol: string) =>
    setFavorites((prev) => {
      const next = new Set(prev)
      if (next.has(symbol)) next.delete(symbol)
      else next.add(symbol)
      return next
    })
  return { favorites, toggle }
}

export function MarketsPage() {
  const [query, setQuery] = useState('')
  const [chosen, setChosen] = useState<string | null>(null)
  const [venue, setVenue] = useState<string | null>(null)
  const [market, setMarket] = useState<MarketType>('perp')

  const { favorites, toggle } = useFavorites()
  const [onlyFavorites, setOnlyFavorites] = useState(false)

  // The catalogue: names and which venues list them. Refreshed slowly — a
  // listing appearing is a rare event, and the numbers arrive on the socket.
  const catalogue = useQuery(() => api.markets({ q: query || undefined }), [query], 30_000)
  const found: Market[] = catalogue.state === 'ready' ? catalogue.data : []
  // Starred pairs float to the top rather than into a separate list: a desk
  // watching six markets still wants the seventh to be one scroll away.
  const markets = useMemo(() => {
    const kept = onlyFavorites ? found.filter((m) => favorites.has(m.symbol)) : found
    return [...kept].sort(
      (a, b) => Number(favorites.has(b.symbol)) - Number(favorites.has(a.symbol)),
    )
  }, [found, favorites, onlyFavorites])
  const selected = useMemo(
    () => markets.find((m) => m.symbol === chosen) ?? markets[0] ?? null,
    [markets, chosen],
  )
  const counts = useMemo(() => {
    const out = new Map<MarketType, number>()
    for (const l of selected?.listings ?? []) out.set(l.market, (out.get(l.market) ?? 0) + 1)
    return out
  }, [selected])
  const shownMarket: MarketType =
    (counts.get(market) ?? 0) > 0
      ? market
      : (k_market_order.find((m) => (counts.get(m) ?? 0) > 0) ?? market)
  const listings = useMemo(
    () => (selected?.listings ?? []).filter((l) => l.market === shownMarket),
    [selected, shownMarket],
  )
  // Only the open market is subscribed. Subscribing to the whole catalogue
  // would be thousands of instruments to read sixteen rows.
  const shown = useMemo(() => listings.map((l) => l.instrument_id), [listings])
  const quotes = useQuotes(shown)
  const funding = useFunding(shown)
  const ticker = useTicker(shown)

  const live: LiveMarket | null = useMemo(() => {
    if (selected === null) return null
    return {
      symbol: selected.symbol,
      listings: listings.map((l) => {
        const q = quotes.get(l.instrument_id)
        const f = funding.get(l.instrument_id)
        const t = ticker.get(l.instrument_id)
        const seconds = f?.interval_seconds ?? null
        return {
          ...l,
          bid: num(q?.bid),
          ask: num(q?.ask),
          volume24h: num(t?.volume_24h_quote),
          fundingRate: num(f?.rate),
          // NULL, not a default of eight: a venue that does not state its
          // interval cannot have its rate normalised, and guessing one is how
          // hyperliquid's hourly rate reads as eight times itself.
          fundingIntervalHours: seconds === null || seconds === 0 ? null : seconds / 3600,
        }
      }),
    }
  }, [selected, listings, quotes, funding, ticker])
  const entering = useEntranceOnce(live !== null, live?.symbol)

  // The row you clicked is what the ticket writes against and what the ladder
  // shows. Falling back to the first listing means the right column is never
  // empty on a market you just opened.
  const [sort, setSort] = useState<{ key: SortKey; desc: boolean }>({ key: 'venue', desc: false })

  const chosenVenue = live?.listings.find((l) => l.venue === venue) ?? live?.listings[0] ?? null
  const book = useBook(chosenVenue?.instrument_id ?? null, 12)

  const ordered = [...(live?.listings ?? [])].sort((a, b) => {
    const x = sortValue(a, sort.key)
    const y = sortValue(b, sort.key)
    // A venue that has said nothing sinks, whichever way the column runs.
    if (x === null && y === null) return 0
    if (x === null) return 1
    if (y === null) return -1
    const c = typeof x === 'string' ? x.localeCompare(y as string) : x - (y as number)
    return sort.desc ? -c : c
  })

  return (
    <div className="flex h-full min-h-0 gap-2">
      <MarketList
        markets={markets}
        selected={selected?.symbol ?? null}
        query={query}
        entering={entering}
        favorites={favorites}
        total={found.length}
        onFavorite={toggle}
        onlyFavorites={onlyFavorites}
        onToggleFilter={() => setOnlyFavorites((v) => !v)}
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
        <section className="panel flex min-h-0 flex-[3] flex-col">
          <MarketHeader
            symbol={live?.symbol ?? '—'}
            market={shownMarket}
            counts={counts}
            onMarket={(m) => {
              setMarket(m)
              setVenue(null)
            }}
          />
          <div className="min-h-0 flex-1 overflow-auto">
            <table className="w-full border-collapse text-sm whitespace-nowrap">
              <thead className="sticky top-0 z-10 bg-panel">
                <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                  <SortHead k="venue" sort={sort} onSort={setSort}>
                    Venue
                  </SortHead>
                  <th className="col-head px-3 text-left">Symbol</th>
                  <SortHead k="bid" sort={sort} onSort={setSort} right>
                    Bid
                  </SortHead>
                  <SortHead k="ask" sort={sort} onSort={setSort} right>
                    Ask
                  </SortHead>
                  <SortHead k="spread" sort={sort} onSort={setSort} right>
                    Spread bp
                  </SortHead>
                  <SortHead k="volume" sort={sort} onSort={setSort} right>
                    Volume 24h
                  </SortHead>
                  <SortHead k="funding" sort={sort} onSort={setSort} right>
                    Funding
                  </SortHead>
                  <th className="col-head px-3 text-right">Every</th>
                </tr>
              </thead>
              <tbody>
                {ordered.map((l, i) => (
                  <VenueRow
                    key={l.instrument_id}
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

        {/* Three to two in favour of the board: it is what the screen is for,
            and the log below it is read a row at a time rather than scanned. */}
        {/* Balances are not a market's business — they live in Treasury, and
            a fourth tab here only widens the strip you tab across. */}
        <div className="flex min-h-0 flex-[2] flex-col">
          <Blotter tabs={['positions', 'orders', 'trades']} />
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
          {book === null ? (
            <EmptyState title="No book yet" />
          ) : (
            <Ladder
              bids={book.bids.map(toLevel)}
              asks={book.asks.map(toLevel)}
            />
          )}
        </section>
      </div>
    </div>
  )
}

/** The active market, stated once and compactly.
 *
 *  Three facts on two lines rather than five chips on one: what the pair is,
 *  what kind of contract it is, and how many venues are answering. */
/** The quote half of a pair, dimmed so the eye reads down the base column.
 *
 *  READ OFF THE SYMBOL, never assumed. This used to strip a literal '/USDT'
 *  and print one back, which is right only while every listing is USDT-quoted
 *  — and the moment spot gateways joined, AAVE/BTC was drawn "AAVE/BTC/USDT",
 *  AAVE/EUR "AAVE/EUR/USDT", and hyperliquid's bare AAVE "AAVE/USDT". A label
 *  that names the wrong quote currency names a different market.
 */
function quote(symbol: string): string {
  const cut = symbol.lastIndexOf('/')
  return cut < 0 ? '' : symbol.slice(cut)
}

function base(symbol: string): string {
  const cut = symbol.lastIndexOf('/')
  return cut < 0 ? symbol : symbol.slice(0, cut)
}

function MarketHeader({
  symbol,
  market,
  counts,
  onMarket,
}: {
  symbol: string
  market: MarketType
  counts: Map<MarketType, number>
  onMarket: (market: MarketType) => void
}) {
  const offered = k_market_order.filter((m) => (counts.get(m) ?? 0) > 0)
  return (
    <div className="flex h-9 shrink-0 items-center gap-2.5 px-3">
      <span className="text-sm font-semibold tracking-[-0.01em] text-fg">{symbol}</span>
      <span className="text-2xs text-fg-dim uppercase">{market}</span>
      <div className="ml-auto flex items-center gap-4" role="tablist">
        {offered.map((m) => (
          <button
            key={m}
            type="button"
            role="tab"
            aria-selected={m === market}
            onClick={() => onMarket(m)}
            className={cn(
              'relative flex h-9 items-center gap-1.5 text-xs capitalize transition-colors duration-150',
              m === market
                ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                : 'text-fg-dim hover:text-fg-mid',
            )}
          >
            {m}
            <span className="num text-2xs text-fg-dim">{counts.get(m)}</span>
          </button>
        ))}
      </div>
      <span className="size-[5px] rounded-full bg-up" />
    </div>
  )
}

function MarketList({
  markets,
  selected,
  query,
  entering,
  favorites,
  total,
  onFavorite,
  onlyFavorites,
  onToggleFilter,
  onQuery,
  onPick,
}: {
  markets: Market[]
  selected: string | null
  query: string
  entering: boolean
  favorites: Set<string>
  /** The catalogue before the favorites filter. The All tab has to count what
   *  it would show, not what the current tab is showing. */
  total: number
  onFavorite: (symbol: string) => void
  onlyFavorites: boolean
  onToggleFilter: () => void
  onQuery: (q: string) => void
  onPick: (symbol: string) => void
}) {
  return (
    <section className="panel flex w-68 shrink-0 flex-col">
      {/* Two views of one list rather than a toggle: a filter you switch on
          and forget looks like an empty catalogue the next time you open the
          screen, where a tab tells you which of the two you are in. */}
      <div className="flex h-7 shrink-0 items-center gap-4 px-3">
        {([false, true] as const).map((only) => (
          <button
            key={String(only)}
            type="button"
            onClick={() => {
              if (onlyFavorites !== only) onToggleFilter()
            }}
            className={cn(
              'relative flex h-7 items-center gap-1.5 text-xs transition-colors duration-150',
              onlyFavorites === only
                ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                : 'text-fg-dim hover:text-fg-mid',
            )}
          >
            {only ? 'Favorites' : 'All'}
            <span className="num text-2xs text-fg-dim">
              {only ? favorites.size : total}
            </span>
          </button>
        ))}
      </div>
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
            const starred = favorites.has(m.symbol)
            return (
            <li key={m.symbol} className="group relative">
              {/* The star sits over the row rather than in a column of its own:
                  a column that is empty on most rows is a column of nothing. */}
              <button
                type="button"
                onClick={() => onFavorite(m.symbol)}
                title={starred ? 'Remove from favorites' : 'Add to favorites'}
                className={cn(
                  'absolute top-1/2 left-1.5 z-10 -translate-y-1/2 text-xs transition-opacity duration-150',
                  starred
                    ? 'text-amber opacity-100'
                    : 'text-fg-dim opacity-0 group-hover:opacity-100 hover:text-fg-mid',
                )}
              >
                ★
              </button>
              <button
                type="button"
                onClick={() => onPick(m.symbol)}
                style={entering ? enterDelay(i) : undefined}
                className={cn(
                  'relative flex h-7 w-full items-center justify-between overflow-hidden rounded-sm pr-2 pl-5',
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
                  {base(m.symbol)}
                  <span className="text-fg-dim">{quote(m.symbol)}</span>
                </span>
                <span className="flex shrink-0 items-baseline gap-3">
                  {/* The count is why this view exists: a market four venues
                      quote is worth opening, a market one venue lists is not.
                      NO PRICE HERE. Only the open market is subscribed —
                      quoting the whole catalogue would be thousands of live
                      instruments to fill a sidebar — and a stale or invented
                      figure beside a name is worse than none. */}
                  <span className="num w-3 text-right text-2xs text-fg-dim">
                    {venueCount(m)}
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
  listing: LiveListing
  selected: boolean
  onSelect: () => void
  style: React.CSSProperties | undefined
  entering: boolean
}) {
  // Basis points of the mid, not currency: two venues quoting one market at
  // different tick sizes are not comparable in their own units. Null until
  // BOTH sides have quoted — half a spread is not a narrow one.
  const spread =
    listing.bid === null || listing.ask === null || listing.bid + listing.ask === 0
      ? null
      : ((listing.ask - listing.bid) / ((listing.bid + listing.ask) / 2)) * 10_000
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
        <Venue venue={listing.venue} />
      </td>
      <td className="px-3 text-xs text-fg-mid">{listing.exchange_symbol}</td>
      {/* An em-dash where the venue has not quoted. A zero would be a price. */}
      <td className="px-3 text-right">
        {listing.bid === null ? (
          <Silent />
        ) : (
          <FlashPrice value={listing.bid} format={price} className="text-up" />
        )}
      </td>
      <td className="px-3 text-right">
        {listing.ask === null ? (
          <Silent />
        ) : (
          <FlashPrice value={listing.ask} format={price} className="text-down" />
        )}
      </td>
      <td className="num px-3 text-right text-fg-mid">
        {spread === null ? <Silent /> : spread.toFixed(1)}
      </td>
      {/* The quote-currency column. Dimmed, because it is context for the two
          numbers a trade is decided on rather than one of them. */}
      <td className="num px-3 text-right text-fg-mid">
        {listing.volume24h === null ? <Silent /> : compact(listing.volume24h)}
      </td>
      <td className="num px-3 text-right">
        {/* The sign is always printed, so a positive rate occupies the same
            width as a negative one and the column's digits line up.
            Colour marks only the negative: paying to be long is the exception
            worth finding, and a column that is mostly green is a column the
            eye stops reading. */}
        {listing.fundingRate === null ? (
          <Silent />
        ) : (
          <span className={listing.fundingRate < 0 ? 'text-down' : 'text-fg'}>
            {listing.fundingRate >= 0 ? '+' : '−'}
            {(Math.abs(listing.fundingRate) * 100).toFixed(4)}%
          </span>
        )}
      </td>
      {/* The interval travels with the rate, in a column of its own. okx funds
          every four hours and hyperliquid every one where the others fund every
          eight; two identical rates side by side without it compare different
          things. */}
      <td className="num px-3 text-right text-fg-dim">
        {listing.fundingIntervalHours === null ? <Silent /> : `${listing.fundingIntervalHours}h`}
      </td>
    </tr>
  )
}

/** A book level as the ladder draws it. The wire carries decimal STRINGS —
 *  a price and a size are exact there and must not be compared as text — and a
 *  level that will not parse is dropped to zero size rather than dropped from
 *  the book, so the ladder's depth stays monotonic. */
const toLevel = (l: { price: string; size: string }) => ({
  price: Number(l.price),
  size: Number(l.size) || 0,
})

/** What a venue that has published nothing looks like. One component, so the
 *  distinction between "no figure" and "a figure of zero" is drawn the same
 *  way everywhere rather than re-invented per column. */
function Silent() {
  return <span className="text-fg-dim" title="the venue has published no figure">—</span>
}

/** A column header that sorts. The caret shows only on the active column —
 *  eight carets in a header row is a row of decoration. */
function SortHead({
  k,
  sort,
  onSort,
  right,
  children,
}: {
  k: SortKey
  sort: { key: SortKey; desc: boolean }
  onSort: (s: { key: SortKey; desc: boolean }) => void
  right?: boolean
  children: React.ReactNode
}) {
  const active = sort.key === k
  return (
    <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left')}>
      <button
        type="button"
        onClick={() => onSort({ key: k, desc: active ? !sort.desc : true })}
        className={cn(
          'inline-flex items-center gap-1 transition-colors duration-150 hover:text-fg-mid',
          active && 'text-fg-mid',
        )}
      >
        {children}
        {active && <span className="text-[8px] leading-none">{sort.desc ? '▼' : '▲'}</span>}
      </button>
    </th>
  )
}
