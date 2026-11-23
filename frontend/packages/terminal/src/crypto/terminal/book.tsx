import { useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import { cn, ExchangeIcon, venueLabel } from '@raikiri/ui'
import { compact, Corners, Empty, Flash, merged, n, Seg, sums, type Level, type Sum } from '@raikiri/ui/v3'
import type { Book } from '../api/client'
import { bp, px, qty, venueColor, type Leg } from '../lib/market'
import { useTerminal, type BookVenue } from './state'

function levelsOf(book: Book | undefined, leg: Leg, side: 'bids' | 'asks'): Level[] {
  const out: Level[] = []
  if (!leg.sized) return out
  for (const l of book?.[side] ?? []) {
    const price = n(l.price)
    const size = n(l.size)
    if (price === undefined || size === undefined || size <= 0) continue
    out.push({ price, size: size * leg.unit, parts: { [leg.venue]: size * leg.unit } })
  }
  return out
}

/** the ladder of one venue, or of every venue summed by price, sizes in the base asset */
export function useBook(venue: BookVenue) {
  const { pair, books } = useTerminal()
  return useMemo(() => {
    const legs = (pair?.legs ?? []).filter((l) => venue === 'all' || l.venue === venue)
    const raw = (side: 'bids' | 'asks') => legs.map((l) => levelsOf(books.get(l.id), l, side))
    if (venue !== 'all') {
      const places = Math.max(0, ...legs.map((l) => l.places))
      const has = legs.some((l) => books.has(l.id))
      return { bids: merged(raw('bids'), 'bids', 10 ** places), asks: merged(raw('asks'), 'asks', 10 ** places), has, places, legs }
    }
    // Venues tick in different sizes, down to a hundred-thousandth of a dollar, so summed as quoted no two levels
    // meet and the ladder reads as noise. Every price goes to a step of five significant figures instead: a bid
    // down to it and an ask up, so no level looks better than it is.
    const all = { bids: raw('bids'), asks: raw('asks') }
    const near = all.bids.find((b) => b.length > 0)?.[0]?.price ?? all.asks.find((a) => a.length > 0)?.[0]?.price ?? 1
    const step = 10 ** (Math.floor(Math.log10(near)) - 4)
    const places = Math.max(0, -Math.round(Math.log10(step)))
    const to = (side: 'bids' | 'asks') =>
      all[side].map((book) =>
        book.map((l) => ({
          ...l,
          // the nudge keeps a price already on the step from slipping a whole step on a float's last digit
          price: (side === 'bids' ? Math.floor(l.price / step + 1e-9) : Math.ceil(l.price / step - 1e-9)) * step,
        })),
      )
    const bids = merged(to('bids'), 'bids', 1 / step)
    const asks = merged(to('asks'), 'asks', 1 / step)
    const has = legs.some((l) => books.has(l.id))
    return { bids, asks, has, places, legs }
  }, [pair, books, venue])
}

export function BookTools() {
  const { pair, leg, bookVenue, setBookVenue } = useTerminal()
  if (pair === undefined || pair.legs.length < 2) return null
  return (
    <Seg
      size="xs"
      value={bookVenue === 'all' ? 'all' : 'one'}
      onChange={(v) => setBookVenue(v === 'all' ? 'all' : (leg?.venue ?? 'all'))}
      options={[
        [
          'one',
          <span key="one" className="flex items-center gap-1.5">
            {leg !== undefined && <ExchangeIcon venue={leg.venue} className="size-3.5" />}
            {leg === undefined ? 'One' : venueLabel(leg.venue)}
          </span>,
        ],
        ['all', 'Combined'],
      ]}
    />
  )
}

export function BookWidget() {
  const { pair, bookVenue, setDraft } = useTerminal()
  const book = useBook(bookVenue)
  const [hover, setHover] = useState<{ side: 'asks' | 'bids'; i: number; rect: DOMRect } | null>(null)
  const askSums = useMemo(() => sums(book.asks), [book.asks])
  const bidSums = useMemo(() => sums(book.bids), [book.bids])
  const deepest = Math.max(askSums.at(-1)?.usd ?? 0, bidSums.at(-1)?.usd ?? 0, 1)
  const biggest = Math.max(1e-12, ...book.asks.map((l) => l.size), ...book.bids.map((l) => l.size))
  const bestAsk = book.asks[0]?.price
  const bestBid = book.bids[0]?.price
  const spread = bestAsk !== undefined && bestBid !== undefined ? bestAsk - bestBid : undefined
  const mid = bestAsk !== undefined && bestBid !== undefined ? (bestAsk + bestBid) / 2 : undefined
  const several = book.legs.length > 1

  if (pair === undefined) return <Empty title="Pick a market" />

  const rows = (levels: Level[], s: Sum[], which: 'asks' | 'bids') =>
    levels.map((l, i) => {
      const lit = hover?.side === which && i <= hover.i
      const up = which === 'bids'
      const sum = s[i] as Sum
      return (
        <div
          key={l.price}
          role="button"
          tabIndex={-1}
          onPointerEnter={(e) => setHover({ side: which, i, rect: e.currentTarget.getBoundingClientRect() })}
          onPointerLeave={() => setHover(null)}
          onClick={() => setDraft({ action: which === 'asks' ? 'buy' : 'sell', price: l.price })}
          className={cn(
            'relative grid h-[22px] shrink-0 cursor-pointer grid-cols-[1.2fr_1fr_1fr] items-center px-3 text-xs transition-colors duration-100',
            lit ? (up ? 'bg-up/[0.07]' : 'bg-down/[0.07]') : 'hover:bg-hover/50',
          )}
        >
          <span
            className={cn(
              'absolute inset-y-px left-0 w-full origin-left transition-transform duration-300 ease-out',
              up ? 'bg-up/[0.08]' : 'bg-down/[0.08]',
            )}
            style={{ transform: `scaleX(${sum.usd / deepest})` }}
          />
          {several ? (
            <span
              className="absolute inset-y-[5px] left-0 flex w-full origin-left transition-transform duration-300 ease-out"
              style={{ transform: `scaleX(${l.size / biggest})` }}
            >
              {book.legs.map((leg) =>
                l.parts[leg.venue] === undefined ? null : (
                  <span
                    key={leg.venue}
                    className="h-full opacity-30"
                    style={{
                      width: `${((l.parts[leg.venue] ?? 0) / l.size) * 100}%`,
                      background: venueColor(leg.venue),
                    }}
                  />
                ),
              )}
            </span>
          ) : (
            <span
              className={cn(
                'absolute inset-y-[5px] left-0 w-full origin-left transition-transform duration-300 ease-out',
                up ? 'bg-up/20' : 'bg-down/20',
              )}
              style={{ transform: `scaleX(${l.size / biggest})` }}
            />
          )}
          <span
            className={cn(
              'mono relative flex items-center gap-1',
              up ? 'text-up' : 'text-down',
              i === 0 && 'font-semibold',
            )}
          >
            {px(l.price, book.places)}
            {several &&
              book.legs.map((leg) =>
                l.parts[leg.venue] === undefined ? null : (
                  <span key={leg.venue} className="size-1 rounded-full" style={{ background: venueColor(leg.venue) }} />
                ),
              )}
          </span>
          <span className="mono relative text-right text-fg-mid">
            <Flash value={l.size}>{qty(l.size)}</Flash>
          </span>
          <span className={cn('mono relative text-right', lit ? 'text-fg' : 'text-fg-dim')}>
            {compact(sum.usd, true)}
          </span>
        </div>
      )
    })

  const tip = hover === null ? undefined : (hover.side === 'asks' ? askSums : bidSums)[hover.i]
  const levels = hover === null ? [] : (hover.side === 'asks' ? book.asks : book.bids).slice(0, hover.i + 1)

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="flex shrink-0 items-center justify-between gap-1.5 px-2 py-1.5">
        <span className="flex items-baseline gap-1 pl-1 text-xs">
          <span className="font-medium text-fg">{pair.base}</span>
          <span className="text-[10px] text-fg-dim">{bookVenue === 'all' ? 'combined' : pair.quote}</span>
        </span>
        <BookTools />
      </div>
      <div className="col-head grid shrink-0 grid-cols-[1.2fr_1fr_1fr] px-3 pb-1">
        <span>Price</span>
        <span className="text-right">Size</span>
        <span className="text-right">Total</span>
      </div>
      {!book.has ? (
        <Empty title="No book yet" />
      ) : (
        <>
          <div className="flex min-h-0 flex-1 flex-col-reverse overflow-y-auto [scrollbar-width:thin]">
            {rows(book.asks, askSums, 'asks')}
          </div>
          <div className="relative grid h-8 shrink-0 grid-cols-[1.2fr_1fr_1fr] items-center border-y border-line bg-canvas/50 px-3 text-xs">
            <span className="text-fg-dim">
              Spread <span className="mono text-fg-mid">{spread === undefined ? '—' : px(spread, book.places)}</span>
            </span>
            <span className="mono text-center text-sm font-semibold text-fg">
              <Flash value={mid}>{px(mid, book.places)}</Flash>
            </span>
            <span className={cn('mono text-right', spread !== undefined && spread < 0 ? 'text-up' : 'text-fg-dim')}>
              {spread === undefined || mid === undefined ? '' : bp((spread / mid) * 1e4, 2)}
            </span>
          </div>
          <div className="flex min-h-0 flex-1 flex-col overflow-y-auto [scrollbar-width:thin]">
            {rows(book.bids, bidSums, 'bids')}
          </div>
        </>
      )}
      {hover !== null && tip !== undefined && (
        <BookTip rect={hover.rect} sum={tip} levels={levels} side={hover.side} places={book.places} base={pair.base} />
      )}
    </div>
  )
}

/** what taking every level up to the hovered one costs, beside the book like a tooltip card */
function BookTip({
  rect,
  sum,
  levels,
  side,
  places,
  base,
}: {
  rect: DOMRect
  sum: Sum
  levels: Level[]
  side: 'asks' | 'bids'
  places: number
  base: string
}) {
  const w = 208
  const left = rect.left - w - 10 > 8 ? rect.left - w - 10 : rect.right + 10
  const avg = sum.shares > 0 ? sum.usd / sum.shares : undefined
  const first = levels[0]?.price
  const worst = levels.at(-1)?.price
  const impact = avg !== undefined && first !== undefined ? (Math.abs(avg - first) / first) * 1e4 : undefined
  return createPortal(
    <div
      className="v3 v3-pop pointer-events-none fixed z-50 rounded-sm bg-raised/95 px-3 py-2 text-xs shadow-[0_16px_40px_-8px_rgb(0_0_0/0.85),0_0_0_1px_var(--color-line)] backdrop-blur"
      style={{ top: rect.top + rect.height / 2 - 52, left, width: w }}
    >
      <Corners />
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Avg. price</span>
        <span className={cn('mono', side === 'asks' ? 'text-down' : 'text-up')}>{px(avg, places + 1)}</span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Sum {base}</span>
        <span className="mono text-fg">{qty(sum.shares)}</span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Sum USD</span>
        <span className="mono text-fg">{compact(sum.usd, true)}</span>
      </div>
      <div className="flex justify-between gap-3 py-0.5">
        <span className="text-fg-dim">Impact</span>
        <span className="mono text-fg-mid">{bp(impact, 2)}</span>
      </div>
      <div className="mt-1 border-t border-line pt-1 text-2xs text-fg-dim">
        {levels.length} {levels.length === 1 ? 'level' : 'levels'} · to {px(worst, places)}
      </div>
    </div>,
    document.body,
  )
}
