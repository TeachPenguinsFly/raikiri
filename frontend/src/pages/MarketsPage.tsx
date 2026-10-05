import { memo, useCallback, useEffect, useMemo, useState } from 'react'
import { api, type Market } from '@/api/client'
import type { components } from '@/api/types'
import { EmptyState } from '@/components/shared/EmptyState'
import { FlashPrice } from '@/components/shared/FlashPrice'
import { Icon } from '@/components/shared/Icon'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Silent } from '@/components/shared/Silent'
import { SortHead, type Sort } from '@/components/shared/SortHead'
import { Venue } from '@/components/shared/Venue'
import { Blotter } from '@/components/trading/Blotter'
import { Ladder } from '@/components/trading/Ladder'
import { OrderTicket } from '@/components/trading/OrderTicket'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn, compact, num, placesOf, spreadBp } from '@/lib/format'
import type { LiveListing } from '@/lib/live'
import { useQuery, type Query } from '@/lib/query'
import { useBook, useFunding, useQuotes, useStreamDown, useTicker } from '@/lib/stream'

type MarketType = components['schemas']['MarketType']

type SortKey = 'venue' | 'bid' | 'ask' | 'spread' | 'funding' | 'volume'

const k_market_order: MarketType[] = ['perp', 'spot', 'future', 'option']

const k_catalogue_refresh_ms = 30_000

const k_book_levels = 12

const k_funding_basis_hours = 8

/** favourites are per viewer in localstorage; a failed read starts empty */
const k_favorites = 'raikiri.favorites'

const k_no_markets: Market[] = []

const venueCount = (m: Market) => new Set(m.listings.map((l) => l.venue)).size

/** a venue that has not quoted sorts last in either direction */
const sortValue = (l: LiveListing, key: SortKey): number | string | null => {
  switch (key) {
    case 'venue':
      return l.venue
    case 'bid':
      return l.bidValue
    case 'ask':
      return l.askValue
    case 'spread':
      return spreadBp(l.bidValue, l.askValue)
    // venues fund every 1, 4 or 8 hours, so the rate is normalised before comparing
    case 'funding':
      return l.fundingRate === null || l.fundingIntervalHours === null
        ? null
        : (l.fundingRate / l.fundingIntervalHours) * k_funding_basis_hours
    case 'volume':
      return l.volume24h
  }
}

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
      // storage can be blocked; favourites then last for the session
    }
  }, [favorites])

  const toggle = useCallback(
    (symbol: string) =>
      setFavorites((prev) => {
        const next = new Set(prev)
        if (next.has(symbol)) next.delete(symbol)
        else next.add(symbol)
        return next
      }),
    [],
  )
  return { favorites, toggle }
}

export function MarketsPage() {
  const [query, setQuery] = useState('')
  const [chosen, setChosen] = useState<string | null>(null)
  const [venue, setVenue] = useState<string | null>(null)
  const [market, setMarket] = useState<MarketType>('perp')
  const [sort, setSort] = useState<Sort<SortKey>>({ key: 'venue', desc: false })
  const [onlyFavorites, setOnlyFavorites] = useState(false)
  const { favorites, toggle } = useFavorites()

  const catalogue = useQuery(() => api.markets(query || undefined), [query], k_catalogue_refresh_ms)
  const found = catalogue.state === 'ready' ? catalogue.data : k_no_markets
  const markets = useMemo(() => {
    const kept = onlyFavorites ? found.filter((m) => favorites.has(m.key)) : found
    return [...kept].sort(
      (a, b) => Number(favorites.has(b.key)) - Number(favorites.has(a.key)),
    )
  }, [found, favorites, onlyFavorites])
  const selected = markets.find((m) => m.key === chosen) ?? markets[0] ?? null

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
  const shown = useMemo(() => listings.map((l) => l.instrument_id), [listings])
  const quotes = useQuotes(shown)
  const funding = useFunding(shown)
  const ticker = useTicker(shown)
  const streamDown = useStreamDown()

  const live: LiveListing[] = useMemo(
    () =>
      listings.map((l) => {
        const q = quotes.get(l.instrument_id)
        const f = funding.get(l.instrument_id)
        const seconds = f?.interval_seconds ?? null
        return {
          ...l,
          bid: q?.bid || null,
          ask: q?.ask || null,
          bidValue: num(q?.bid),
          askValue: num(q?.ask),
          volume24h: num(ticker.get(l.instrument_id)?.volume_24h_quote),
          fundingRate: num(f?.rate),
          fundingIntervalHours: seconds === null || seconds === 0 ? null : seconds / 3600,
        }
      }),
    [listings, quotes, funding, ticker],
  )
  const entering = useEntranceOnce(selected !== null, selected?.key)

  const places = placesOf(live.flatMap((l) => [l.bid, l.ask]))

  const chosenVenue = live.find((l) => l.venue === venue) ?? live[0] ?? null
  const book = useBook(chosenVenue?.instrument_id ?? null, k_book_levels)

  const ordered = [...live].sort((a, b) => {
    const x = sortValue(a, sort.key)
    const y = sortValue(b, sort.key)
    if (x === null && y === null) return 0
    if (x === null) return 1
    if (y === null) return -1
    const c = typeof x === 'string' ? x.localeCompare(y as string) : x - (y as number)
    return sort.desc ? -c : c
  })

  const onQuery = useCallback((q: string) => {
    setQuery(q)
    setChosen(null)
  }, [])
  const onPick = useCallback((symbol: string) => {
    setChosen(symbol)
    setVenue(null)
  }, [])

  return (
    <div className="flex h-full min-h-0 gap-2">
      <MarketList
        catalogue={catalogue}
        markets={markets}
        selected={selected?.key ?? null}
        query={query}
        entering={entering}
        favorites={favorites}
        total={found.length}
        onFavorite={toggle}
        onlyFavorites={onlyFavorites}
        onOnlyFavorites={setOnlyFavorites}
        onQuery={onQuery}
        onPick={onPick}
      />

      <div className="flex min-w-0 flex-1 flex-col gap-2">
        <section className="panel flex min-h-0 flex-[3] flex-col">
          <MarketHeader
            symbol={selected?.symbol ?? '—'}
            market={shownMarket}
            counts={counts}
            streamDown={streamDown}
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
                    places={places}
                    selected={chosenVenue?.venue === l.venue}
                    onSelect={() => setVenue(l.venue)}
                    style={entering ? enterDelay(i) : undefined}
                    entering={entering}
                  />
                ))}
              </tbody>
            </table>
            {catalogue.state === 'failed' && (
              <EmptyState title="Catalogue unavailable" hint={catalogue.title} />
            )}
          </div>
        </section>

        <div className="flex min-h-0 flex-[2] flex-col">
          <Blotter />
        </div>
      </div>

      <div className="flex w-64 shrink-0 flex-col gap-2">
        <OrderTicket listings={live} venue={chosenVenue?.venue ?? null} onVenue={setVenue} />
        <section className="panel flex min-h-0 flex-1 flex-col">
          <PanelHeader
            title="Order book"
            right={
              chosenVenue === null ? null : <span className="meta">{chosenVenue.venue}</span>
            }
          />
          {book !== null ? (
            <Ladder book={book} />
          ) : (
            <EmptyState
              title={streamDown && chosenVenue !== null ? 'Stream unavailable' : 'No book yet'}
            />
          )}
        </section>
      </div>
    </div>
  )
}

/** listings are not all usdt-quoted, so the quote currency is read off the symbol */
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
  streamDown,
  onMarket,
}: {
  symbol: string
  market: MarketType
  counts: Map<MarketType, number>
  streamDown: boolean
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
      <span
        className={cn('size-[5px] rounded-full', streamDown ? 'bg-down' : 'bg-up')}
        title={streamDown ? 'stream unavailable' : undefined}
      />
    </div>
  )
}

/** memo: the list holds no prices, so a tick must not redraw it */
const MarketList = memo(function MarketList({
  catalogue,
  markets,
  selected,
  query,
  entering,
  favorites,
  total,
  onFavorite,
  onlyFavorites,
  onOnlyFavorites,
  onQuery,
  onPick,
}: {
  catalogue: Query<Market[]>
  markets: Market[]
  selected: string | null
  query: string
  entering: boolean
  favorites: Set<string>
  /** count before the favourites filter, for the all tab */
  total: number
  onFavorite: (symbol: string) => void
  onlyFavorites: boolean
  onOnlyFavorites: (only: boolean) => void
  onQuery: (q: string) => void
  onPick: (symbol: string) => void
}) {
  return (
    <section className="panel flex w-68 shrink-0 flex-col">
      <div className="flex h-7 shrink-0 items-center gap-4 px-3">
        {([false, true] as const).map((only) => (
          <button
            key={String(only)}
            type="button"
            onClick={() => onOnlyFavorites(only)}
            className={cn(
              'relative flex h-7 items-center gap-1.5 text-xs transition-colors duration-150',
              onlyFavorites === only
                ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                : 'text-fg-dim hover:text-fg-mid',
            )}
          >
            {only ? 'Favorites' : 'All'}
            <span className="num text-2xs text-fg-dim">{only ? favorites.size : total}</span>
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
              'focus:shadow-[inset_0_0_0_1px_color-mix(in_srgb,var(--color-accent)_55%,transparent)]',
            )}
          />
        </div>
      </div>

      <ul className="min-h-0 flex-1 overflow-auto px-1.5 pb-2">
        {markets.map((m, i) => {
          const active = m.key === selected
          const starred = favorites.has(m.key)
          return (
            <li key={m.key} className="group relative">
              <button
                type="button"
                onClick={() => onFavorite(m.key)}
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
                onClick={() => onPick(m.key)}
                style={entering ? enterDelay(i) : undefined}
                className={cn(
                  'relative flex h-7 w-full items-center justify-between overflow-hidden rounded-sm pr-2 pl-5',
                  'transition-colors duration-150',
                  entering && 'enter-fade-up',
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
                  <span className="num w-3 text-right text-2xs text-fg-dim">{venueCount(m)}</span>
                </span>
              </button>
            </li>
          )
        })}
        {catalogue.state === 'loading' && <EmptyState title="Asking admind" />}
        {catalogue.state === 'failed' && (
          <EmptyState title="Catalogue unavailable" hint={catalogue.title} />
        )}
        {catalogue.state === 'ready' && markets.length === 0 && (
          <EmptyState
            title="Nothing matches"
            hint="The catalogue is what the attached gateways loaded."
          />
        )}
      </ul>
    </section>
  )
})

function VenueRow({
  listing,
  places,
  selected,
  onSelect,
  style,
  entering,
}: {
  listing: LiveListing
  places: number
  selected: boolean
  onSelect: () => void
  style: React.CSSProperties | undefined
  entering: boolean
}) {
  const spread = spreadBp(listing.bidValue, listing.askValue)
  const absent = 'the venue has published no figure'
  return (
    <tr
      style={style}
      onClick={onSelect}
      className={cn(
        'row cursor-pointer',
        entering && 'enter-fade-up',
        selected && 'bg-active shadow-[inset_2px_0_0_0_var(--color-accent)] hover:bg-active',
      )}
    >
      <td className="px-3">
        <Venue venue={listing.venue} />
      </td>
      <td className="px-3 text-xs text-fg-mid">{listing.exchange_symbol}</td>
      <td className="px-3 text-right">
        {listing.bid === null ? (
          <Silent title={absent} />
        ) : (
          <FlashPrice value={listing.bid} places={places} className="text-up" />
        )}
      </td>
      <td className="px-3 text-right">
        {listing.ask === null ? (
          <Silent title={absent} />
        ) : (
          <FlashPrice value={listing.ask} places={places} className="text-down" />
        )}
      </td>
      <td className="num px-3 text-right text-fg-mid">
        {spread === null ? <Silent title={absent} /> : spread.toFixed(1)}
      </td>
      <td className="num px-3 text-right text-fg-mid">
        {listing.volume24h === null ? <Silent title={absent} /> : compact(listing.volume24h)}
      </td>
      <td className="num px-3 text-right">
        {/* the sign is always printed so digits line up; colour marks only negatives */}
        {listing.fundingRate === null ? (
          <Silent title={absent} />
        ) : (
          <span className={listing.fundingRate < 0 ? 'text-down' : 'text-fg'}>
            {listing.fundingRate >= 0 ? '+' : '−'}
            {(Math.abs(listing.fundingRate) * 100).toFixed(4)}%
          </span>
        )}
      </td>
      <td className="num px-3 text-right text-fg-dim">
        {listing.fundingIntervalHours === null ? (
          <Silent title={absent} />
        ) : (
          `${listing.fundingIntervalHours}h`
        )}
      </td>
    </tr>
  )
}
