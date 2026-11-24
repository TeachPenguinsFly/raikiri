import { useEffect, useMemo, useRef, useState } from 'react'
import { useNavigate } from 'react-router'
import { cn, ExchangeIcon } from '@raikiri/ui'
import { CoinIcon, coinName, compact, Flash, Glyph, n } from '@raikiri/ui/v3'
import { api as options } from '../../api/client'
import { api, type CandlePage } from '../api/client'
import { useQuotes, useTicker } from '../api/stream'
import { px, touchOf, useQuery, useRanked, usd, type Leg, type Pair } from '../lib/market'
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

export function Stat({ label, tone, children }: { label: string; tone?: string; children: React.ReactNode }) {
  return (
    <span className="flex shrink-0 flex-col leading-tight whitespace-nowrap">
      <span className="text-xs text-fg-dim">{label}</span>
      <span className={cn('mono text-sm', tone ?? 'text-fg')}>{children}</span>
    </span>
  )
}

const k_hour_ms = 3_600_000
const k_day_ms = 24 * k_hour_ms
const k_dollars = new Set(['USDT', 'USDC', 'USD'])

/** the last 24 hours from hourly bars, with the live price folded into the range */
function dayOf(page: CandlePage | undefined, last: number | undefined) {
  if (page === undefined) return undefined
  const from = Date.now() - k_day_ms
  // the bars start when the stack did: a shorter history would pass a few hours off as the day
  if (!page.candles.some((c) => c.t * 1000 < from + k_hour_ms)) return undefined
  const bars = page.candles.filter((c) => c.t * 1000 >= from)
  const open = n(bars[0]?.o)
  const close = last ?? n(bars.at(-1)?.c)
  if (open === undefined || open <= 0 || close === undefined) return undefined
  let high = close
  let low = close
  for (const c of bars) {
    const h = n(c.h)
    const l = n(c.l)
    if (h !== undefined && h > high) high = h
    if (l !== undefined && l > 0 && l < low) low = l
  }
  return { change: close - open, pct: ((close - open) / open) * 100, high, low }
}

const two = (v: number) => String(v).padStart(2, '0')

function Countdown({ until }: { until: number }) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1_000)
    return () => clearInterval(t)
  }, [])
  const left = Math.floor((until - now) / 1000)
  if (left <= 0) return null
  return (
    <span className="text-fg-mid">
      {two(Math.floor(left / 3600))}:{two(Math.floor(left / 60) % 60)}:{two(left % 60)}
    </span>
  )
}

export const spanLabel = (seconds: number) =>
  seconds % 3600 === 0 ? `${seconds / 3600}h` : `${Math.round(seconds / 60)}m`

/** what the venue says of the chosen leg; a number it does not give has no tile */
function Metrics({ pair, leg }: { pair: Pair; leg: Leg }) {
  const { quotes, funding } = useTerminal()
  const ticker = useTicker([leg.id]).get(leg.id)
  const hours = useQuery(`day:${leg.id}`, () => api.candles(leg.id, '1h', 26), 60_000)
  const last = touchOf(quotes.get(leg.id), leg)?.mid
  const f = pair.market === 'perp' ? funding.get(leg.id) : undefined
  const mark = n(f?.mark_price)
  const index = n(f?.index_price)
  const rate = n(f?.rate)
  const next = f?.next_funding_time === undefined ? undefined : Number(f.next_funding_time) / 1e6
  const day = dayOf(hours.state === 'ready' ? hours.data : undefined, last)
  const volume = leg.volume > 0 ? leg.volume : k_dollars.has(leg.quote) ? n(ticker?.volume_24h_quote) : undefined
  const price = mark ?? last
  const interest = n(ticker?.open_interest)
  const sign = day === undefined ? '' : day.change > 0 ? '+' : day.change < 0 ? '−' : ''
  return (
    <>
      {mark !== undefined && <Stat label="Mark">{px(mark, leg.places)}</Stat>}
      {index !== undefined && <Stat label="Index">{px(index, leg.places)}</Stat>}
      {day !== undefined && (
        <>
          <Stat
            label="24h change"
            tone={day.change > 0 ? 'text-up' : day.change < 0 ? 'text-down' : 'text-fg-mid'}
          >
            {sign}
            {px(Math.abs(day.change), leg.places)} ({sign}
            {Math.abs(day.pct).toFixed(2)}%)
          </Stat>
          <Stat label="24h high">{px(day.high, leg.places)}</Stat>
          <Stat label="24h low">{px(day.low, leg.places)}</Stat>
        </>
      )}
      {volume !== undefined && volume > 0 && <Stat label="24h volume">{usd(volume)}</Stat>}
      {interest !== undefined && interest > 0 && price !== undefined && (
        <Stat label="Open interest">{usd(interest * price)}</Stat>
      )}
      {rate !== undefined && (
        <Stat
          label={
            f?.interval_seconds === undefined || f.interval_seconds === null
              ? 'Funding'
              : `Funding · ${spanLabel(f.interval_seconds)}`
          }
        >
          <span className="flex items-baseline gap-2">
            <span className={rate > 0 ? 'text-up' : rate < 0 ? 'text-down' : undefined}>
              {(rate * 100).toFixed(4)}%
            </span>
            {next !== undefined && <Countdown until={next} />}
          </span>
        </Stat>
      )}
    </>
  )
}

/** the chosen market across the top: its name and the market switcher, the venue it is read from, and the numbers an
 *  exchange puts beside the price */
export function MarketBar() {
  const { pair, leg, quotes } = useTerminal()
  const t = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)
  return (
    <div className="flex h-14 shrink-0 items-center gap-5 border-b border-line px-2">
      <MarketPicker />
      <VenuePicker />
      {/* the pickers stay outside the scroller: it would clip the panels they open */}
      <div className="flex min-w-0 flex-1 items-center gap-5 overflow-x-auto [scrollbar-width:none]">
        <Flash value={t?.mid}>
          <span className="mono text-xl font-semibold text-fg">{px(t?.mid, leg?.places ?? 2)}</span>
        </Flash>
        {/* keyed so one leg's day never shows under another's name while the next loads */}
        {pair !== undefined && leg !== undefined && <Metrics key={leg.id} pair={pair} leg={leg} />}
      </div>
    </div>
  )
}
