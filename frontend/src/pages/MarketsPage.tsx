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


// volume ranks by quote turnover; base volume puts inverse contracts on top
type MarketType = components['schemas']['MarketType']

const k_market_order: MarketType[] = ['perp', 'spot', 'future', 'option']

const venueCount = (m: Market) => new Set(m.listings.map((l) => l.venue)).size

type SortKey = 'venue' | 'bid' | 'ask' | 'spread' | 'funding' | 'volume'

/** a venue that has not quoted sorts last in either direction */
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
    // normalised to eight hours; venues fund every 1, 4 or 8 hours
    case 'funding':
      return l.fundingRate === null || l.fundingIntervalHours === null ||
             l.fundingIntervalHours === 0
        ? null
        : (l.fundingRate / l.fundingIntervalHours) * 8
    case 'volume':
      return l.volume24h
  }
}

/** favourites are per viewer in localstorage; a failed read starts empty */
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

  const catalogue = useQuery(() => api.markets({ q: query || undefined }), [query], 30_000)
  const found: Market[] = catalogue.state === 'ready' ? catalogue.data : []
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
          fundingIntervalHours: seconds === null || seconds === 0 ? null : seconds / 3600,
        }
      }),
    }
  }, [selected, listings, quotes, funding, ticker])
  const entering = useEntranceOnce(live !== null, live?.symbol)

  const [sort, setSort] = useState<{ key: SortKey; desc: boolean }>({ key: 'venue', desc: false })

  const chosenVenue = live?.listings.find((l) => l.venue === venue) ?? live?.listings[0] ?? null
  const book = useBook(chosenVenue?.instrument_id ?? null, 12)

  const ordered = [...(live?.listings ?? [])].sort((a, b) => {
    const x = sortValue(a, sort.key)
    const y = sortValue(b, sort.key)
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

/** read the quote currency off the symbol; listings are not all usdt-quoted */
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
  /** count before the favourites filter, for the all tab */
  total: number
  onFavorite: (symbol: string) => void
  onlyFavorites: boolean
  onToggleFilter: () => void
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
                  {/* no price here: only the open market is subscribed */}
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
  // spread in basis points of the mid; null until both sides quote
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
        selected && 'bg-active shadow-[inset_2px_0_0_0_var(--color-accent)] hover:bg-active',
      )}
    >
      <td className="px-3">
        <Venue venue={listing.venue} />
      </td>
      <td className="px-3 text-xs text-fg-mid">{listing.exchange_symbol}</td>
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
      <td className="num px-3 text-right text-fg-mid">
        {listing.volume24h === null ? <Silent /> : compact(listing.volume24h)}
      </td>
      <td className="num px-3 text-right">
        {/* the sign is always printed so digits line up; colour marks only negatives */}
        {listing.fundingRate === null ? (
          <Silent />
        ) : (
          <span className={listing.fundingRate < 0 ? 'text-down' : 'text-fg'}>
            {listing.fundingRate >= 0 ? '+' : '−'}
            {(Math.abs(listing.fundingRate) * 100).toFixed(4)}%
          </span>
        )}
      </td>
      {/* venues fund every 1, 4 or 8 hours, so the interval sits beside the rate */}
      <td className="num px-3 text-right text-fg-dim">
        {listing.fundingIntervalHours === null ? <Silent /> : `${listing.fundingIntervalHours}h`}
      </td>
    </tr>
  )
}

/** an unparseable level gets zero size so depth stays monotonic */
const toLevel = (l: { price: string; size: string }) => ({
  price: Number(l.price),
  size: Number(l.size) || 0,
})

function Silent() {
  return <span className="text-fg-dim" title="the venue has published no figure">—</span>
}

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
