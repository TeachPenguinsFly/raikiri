import { useEffect, useMemo, useRef, useState } from 'react'
import { useNavigate } from 'react-router'
import { cn, ExchangeIcon } from '@raikiri/ui'
import { CoinIcon, coinName, compact, Flash, Glyph, n } from '@raikiri/ui/v3'
import { api as options } from '../../api/client'
import { useQuotes } from '../api/stream'
import { bp, px, touchOf, useQuery, useRanked, type Pair } from '../lib/market'
import { VenuePicker } from './panels'
import { useTerminal } from './state'

type Tab = 'favorites' | 'spot' | 'perp' | 'options'

const k_tabs: ReadonlyArray<readonly [Tab, string]> = [
  ['favorites', 'Favorites'],
  ['spot', 'Spot'],
  ['perp', 'Perpetual'],
  ['options', 'Options'],
]

/** rows drawn before a search narrows the list; each one listens to its price */
const k_rows = 120
const k_open_ms = 120
const k_close_ms = 220
const k_favorites_key = 'raikiri.markets.favorites'

function useFavorites() {
  const [keys, setKeys] = useState<Set<string>>(() => {
    try {
      return new Set(JSON.parse(localStorage.getItem(k_favorites_key) ?? '[]') as string[])
    } catch {
      return new Set()
    }
  })
  const toggle = (key: string) =>
    setKeys((prev) => {
      const next = new Set(prev)
      if (next.has(key)) next.delete(key)
      else next.add(key)
      try {
        localStorage.setItem(k_favorites_key, JSON.stringify([...next]))
      } catch {
        // blocked storage keeps them for the session
      }
      return next
    })
  return { keys, toggle }
}

const kindLabel = (p: Pair) => (p.market === 'perp' ? 'Perpetual' : 'Spot')

/** a few venue marks, then how many more */
function Venues({ pair }: { pair: Pair }) {
  return (
    <span className="flex items-center justify-end gap-0.5">
      {pair.legs.slice(0, 4).map((l) => (
        <ExchangeIcon key={l.venue} venue={l.venue} className="size-3.5" />
      ))}
      {pair.legs.length > 4 && <span className="ml-1 text-xs text-fg-dim">+{pair.legs.length - 4}</span>}
    </span>
  )
}

function MarketRows({
  rows,
  favorites,
  onPick,
}: {
  rows: Pair[]
  favorites: ReturnType<typeof useFavorites>
  onPick: (p: Pair) => void
}) {
  const ids = useMemo(() => rows.flatMap((p) => (p.legs[0] === undefined ? [] : [p.legs[0].id])), [rows])
  const quotes = useQuotes(ids)
  const { pair: current } = useTerminal()
  if (rows.length === 0) return <div className="px-4 py-8 text-center text-sm text-fg-dim">Nothing here yet</div>
  return (
    <>
      {rows.map((p) => {
        const leg = p.legs[0]
        const t = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)
        const starred = favorites.keys.has(p.key)
        return (
          <div
            key={p.key}
            role="button"
            tabIndex={0}
            onClick={() => onPick(p)}
            onKeyDown={(e) => e.key === 'Enter' && onPick(p)}
            className={cn(
              'grid h-12 cursor-pointer grid-cols-[1.25rem_1.75rem_1fr_6.5rem_5.5rem_5.5rem] items-center gap-x-2.5 rounded-sm px-3',
              p.key === current?.key ? 'bg-hover' : 'hover:bg-hover',
            )}
          >
            <button
              type="button"
              aria-label={starred ? 'Remove from favorites' : 'Add to favorites'}
              onClick={(e) => {
                e.stopPropagation()
                favorites.toggle(p.key)
              }}
              className={cn('transition-colors', starred ? 'text-amber' : 'text-fg-dim hover:text-fg-mid')}
            >
              <Glyph name="star" className={cn('size-4', starred && '[&_path]:fill-current')} />
            </button>
            <CoinIcon symbol={p.base} plain={p.stray} className="size-6" />
            <span className="flex min-w-0 flex-col leading-tight">
              <span className="truncate text-sm text-fg">
                <span className="font-medium">{p.base}</span>
                <span className="text-fg-dim">/{p.quote}</span>
              </span>
              <span className="truncate text-xs text-fg-dim">
                {(p.stray ? undefined : coinName(p.base)) ?? kindLabel(p)}
              </span>
            </span>
            <span className="mono text-right text-sm text-fg">{px(t?.mid, leg?.places ?? 2)}</span>
            <span className="mono text-right text-sm text-fg-mid">{p.volume > 0 ? compact(p.volume, true) : '—'}</span>
            <Venues pair={p} />
          </div>
        )
      })}
    </>
  )
}

function OptionRows({ query }: { query: string }) {
  const list = useQuery('option-underlyings', () => options.optionUnderlyings(), 60_000)
  const navigate = useNavigate()
  if (list.state !== 'ready') return <div className="px-4 py-8 text-center text-sm text-fg-dim">Loading</div>
  const q = query.trim().toUpperCase()
  const rows = list.data
    .filter((u) => q === '' || u.underlying.includes(q))
    .sort((a, b) => b.venues.length - a.venues.length || a.underlying.localeCompare(b.underlying))
  return (
    <>
      {rows.map((u) => (
        <div
          key={u.underlying}
          role="button"
          tabIndex={0}
          onClick={() => navigate(`/options?u=${encodeURIComponent(u.underlying)}`)}
          className="grid h-12 cursor-pointer grid-cols-[1.75rem_1fr_6rem_5.5rem] items-center gap-x-2.5 rounded-sm px-3 hover:bg-hover"
        >
          <CoinIcon symbol={u.underlying} className="size-6" />
          <span className="flex min-w-0 flex-col leading-tight">
            <span className="text-sm font-medium text-fg">{u.underlying}</span>
            <span className="truncate text-xs text-fg-dim">{coinName(u.underlying) ?? 'Options'}</span>
          </span>
          <span className="mono text-right text-sm text-fg-mid">
            {u.expiries.length} {u.expiries.length === 1 ? 'expiry' : 'expiries'}
          </span>
          <span className="flex items-center justify-end gap-0.5">
            {u.venues.map((v) => (
              <ExchangeIcon key={v.venue} venue={v.venue} className="size-3.5" />
            ))}
          </span>
        </div>
      ))}
    </>
  )
}

/** every market by kind, busiest first, with a search, favorites and each one's price */
function MarketsPanel({ onDone }: { onDone: () => void }) {
  const { catalog, pair, pick } = useTerminal()
  const ranked = useRanked(catalog.state === 'ready' ? catalog.data : undefined)
  const favorites = useFavorites()
  const [tab, setTab] = useState<Tab>(pair?.market ?? 'perp')
  const [query, setQuery] = useState('')
  const rows = useMemo(() => {
    const q = query.trim().toUpperCase()
    const matching = ranked
      .map((r) => r.pair)
      .filter((p) => q === '' || p.base.includes(q) || (coinName(p.base)?.toUpperCase().includes(q) ?? false))
    const of =
      tab === 'favorites' ? matching.filter((p) => favorites.keys.has(p.key)) : matching.filter((p) => p.market === tab)
    return of.slice(0, k_rows)
  }, [ranked, query, tab, favorites.keys])
  return (
    <div className="flex h-[min(34rem,75vh)] w-[38rem] flex-col">
      <div className="flex h-12 shrink-0 items-center gap-2 border-b border-line px-3">
        <Glyph name="search" className="size-4 text-fg-dim" />
        <input
          autoFocus
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search coins"
          className="min-w-0 flex-1 bg-transparent text-sm text-fg outline-none placeholder:text-fg-dim"
        />
      </div>
      <div className="flex shrink-0 items-center gap-1 px-3 pt-2" role="tablist">
        {k_tabs.map(([t, label]) => (
          <button
            key={t}
            type="button"
            role="tab"
            aria-selected={tab === t}
            onClick={() => setTab(t)}
            className={cn(
              'h-8 rounded-md px-3 text-sm transition-colors',
              tab === t ? 'bg-active text-fg' : 'text-fg-mid hover:bg-hover hover:text-fg',
            )}
          >
            {label}
          </button>
        ))}
      </div>
      {tab !== 'options' && (
        <div className="grid shrink-0 grid-cols-[1.25rem_1.75rem_1fr_6.5rem_5.5rem_5.5rem] gap-x-2.5 px-6 pt-3 pb-1.5 text-xs text-fg-dim">
          <span />
          <span />
          <span>Market</span>
          <span className="text-right">Price</span>
          <span className="text-right">24h volume</span>
          <span className="text-right">Venues</span>
        </div>
      )}
      <div className="min-h-0 flex-1 overflow-y-auto px-3 pb-2">
        {tab === 'options' ? (
          <OptionRows query={query} />
        ) : catalog.state === 'loading' ? (
          <div className="px-4 py-8 text-center text-sm text-fg-dim">Loading markets</div>
        ) : (
          <MarketRows
            rows={rows}
            favorites={favorites}
            onPick={(p) => {
              pick({ market: p.key })
              onDone()
            }}
          />
        )}
      </div>
    </div>
  )
}

/** the market's name: hovering or clicking it opens every other market to switch to */
function MarketPicker() {
  const { pair } = useTerminal()
  const [open, setOpen] = useState(false)
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)
  const box = useRef<HTMLDivElement>(null)
  const later = (next: boolean, ms: number) => {
    clearTimeout(timer.current)
    timer.current = setTimeout(() => setOpen(next), ms)
  }
  useEffect(() => {
    if (!open) return
    const off = (e: MouseEvent) => box.current !== null && !box.current.contains(e.target as Node) && setOpen(false)
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setOpen(false)
    window.addEventListener('mousedown', off)
    window.addEventListener('keydown', esc)
    return () => {
      window.removeEventListener('mousedown', off)
      window.removeEventListener('keydown', esc)
    }
  }, [open])
  useEffect(() => () => clearTimeout(timer.current), [])
  return (
    <div
      ref={box}
      className="relative"
      onMouseEnter={() => later(true, k_open_ms)}
      onMouseLeave={() => later(false, k_close_ms)}
    >
      <button
        type="button"
        aria-expanded={open}
        onClick={() => {
          clearTimeout(timer.current)
          setOpen((o) => !o)
        }}
        className={cn(
          'flex h-11 items-center gap-2.5 rounded-md px-2 transition-colors hover:bg-hover',
          open && 'bg-hover',
        )}
      >
        {pair !== undefined && <CoinIcon symbol={pair.base} plain={pair.stray} className="size-7" />}
        <span className="flex flex-col items-start leading-tight">
          <span className="text-base font-semibold text-fg">
            {pair === undefined ? 'Markets' : `${pair.base}/${pair.quote}`}
          </span>
          <span className="text-xs text-fg-dim">
            {pair === undefined ? 'Pick one' : `${coinName(pair.base) ?? pair.base} · ${kindLabel(pair)}`}
          </span>
        </span>
        <Glyph name="down" className={cn('size-4 text-fg-dim transition-transform', open && 'rotate-180')} />
      </button>
      {open && (
        <div className="v3-pop absolute top-12 left-0 z-50 rounded-md bg-raised shadow-[0_0_0_1px_var(--color-line),0_24px_60px_-12px_rgb(0_0_0/0.9)]">
          <MarketsPanel onDone={() => setOpen(false)} />
        </div>
      )}
    </div>
  )
}

function Stat({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <span className="flex flex-col leading-tight whitespace-nowrap">
      <span className="text-xs text-fg-dim">{label}</span>
      <span className="mono text-sm text-fg">{children}</span>
    </span>
  )
}

/** the chosen market across the top: its name and the market switcher, the venue it is read from, and the numbers that
 *  matter at a glance */
export function MarketBar() {
  const { pair, leg, quotes, funding } = useTerminal()
  const t = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)
  const rate = leg === undefined ? undefined : n(funding.get(leg.id)?.rate)
  return (
    <div className="flex h-14 shrink-0 items-center gap-6 border-b border-line px-2">
      <MarketPicker />
      <VenuePicker />
      <Flash value={t?.mid}>
        <span className="mono text-xl font-semibold text-fg">{px(t?.mid, leg?.places ?? 2)}</span>
      </Flash>
      {pair !== undefined && (
        <>
          <Stat label="Spread">{t === undefined ? '—' : bp(((t.ask - t.bid) / t.mid) * 1e4, 2)}</Stat>
          <Stat label="24h volume">{leg !== undefined && leg.volume > 0 ? compact(leg.volume, true) : '—'}</Stat>
          {pair.market === 'perp' && (
            <Stat label="Funding">{rate === undefined ? '—' : `${(rate * 100).toFixed(4)}%`}</Stat>
          )}
          <Stat label="Venues">{pair.legs.length}</Stat>
        </>
      )}
    </div>
  )
}
