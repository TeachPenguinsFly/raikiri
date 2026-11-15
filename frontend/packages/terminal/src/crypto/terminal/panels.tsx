import { useEffect, useRef, useState } from 'react'
import { cn, ExchangeIcon, venueLabel } from '@raikiri/ui'
import { Empty, Flash, Glyph, n } from '@raikiri/ui/v3'
import { bp, crossOf, px, qty, spreadBp, touchOf, venueColor } from '../lib/market'
import { useTerminal } from './state'

/** the busiest markets with their touch on the first venue, or what the search finds among all of them */
/** the one place the market's venue is chosen; the chart, the book and the ticket follow it. With `lines`, each other
 *  venue also gets a switch for its price line over the candles, in the colour it is drawn in */
export function VenuePicker() {
  const { pair, leg, quotes, pick, hiddenLines, toggleLine, setHiddenLines } = useTerminal()
  const lines = { hidden: hiddenLines, toggle: toggleLine }
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)
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
  if (pair === undefined || leg === undefined) return null
  return (
    <div ref={box} className="relative">
      <button
        type="button"
        aria-expanded={open}
        onClick={() => setOpen((o) => !o)}
        className={cn(
          'flex h-7 cursor-pointer items-center gap-2 rounded-md bg-raised/60 px-2.5 text-sm text-fg transition-colors hover:bg-hover',
          open && 'bg-hover',
        )}
      >
        <ExchangeIcon venue={leg.venue} className="size-4" />
        {venueLabel(leg.venue)}
        {pair.legs.length > 1 && <span className="text-xs text-fg-dim">+{pair.legs.length - 1}</span>}
        <Glyph name="down" className={cn('size-3.5 text-fg-dim transition-transform', open && 'rotate-180')} />
      </button>
      {open && (
        <div className="v3-pop absolute top-9 right-0 z-50 w-96 rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_20px_50px_-10px_rgb(0_0_0/0.9)]">
          <div className="grid grid-cols-[1fr_auto_6.5rem] gap-x-3 px-2.5 pt-2 pb-1.5 text-xs text-fg-dim">
            <span>Venue</span>
            <span className="text-right">Mid</span>
            <span className="flex items-center justify-end gap-1">
              {(
                [
                  ['All', () => setHiddenLines(new Set())],
                  ['None', () => setHiddenLines(new Set(pair.legs.map((l) => l.venue)))],
                ] as const
              ).map(([text, run]) => (
                <button
                  key={text}
                  type="button"
                  onClick={run}
                  className="rounded-sm px-1.5 hover:bg-hover hover:text-fg"
                >
                  {text}
                </button>
              ))}
            </span>
          </div>
          {pair.legs.map((l) => {
            const t = touchOf(quotes.get(l.id), l)
            const chosen = l.venue === leg.venue
            const shown = !lines.hidden.has(l.venue)
            return (
              <div
                key={l.venue}
                className={cn(
                  'grid h-9 grid-cols-[1fr_auto_6.5rem] items-center gap-x-3 rounded-sm px-2.5 text-sm',
                  chosen ? 'bg-hover' : 'hover:bg-hover',
                )}
              >
                <button
                  type="button"
                  onClick={() => {
                    pick({ venue: l.venue })
                    setOpen(false)
                  }}
                  className={cn(
                    'flex min-w-0 items-center gap-2 text-left',
                    chosen ? 'text-fg' : 'text-fg-mid hover:text-fg',
                  )}
                >
                  <ExchangeIcon venue={l.venue} className="size-4" />
                  <span className="truncate">{venueLabel(l.venue)}</span>
                </button>
                <span className="mono text-right text-fg-mid">{px(t?.mid, l.places)}</span>
                {chosen ? (
                  <span className="text-right text-xs text-fg-dim">Candles</span>
                ) : (
                  <button
                    type="button"
                    aria-pressed={shown}
                    aria-label={`${venueLabel(l.venue)} line on the chart`}
                    onClick={() => lines.toggle(l.venue)}
                    className="flex items-center justify-end gap-2"
                  >
                    <span
                      className="h-0.5 w-5 rounded-full transition-opacity"
                      style={{ background: venueColor(l.venue), opacity: shown ? 1 : 0.25 }}
                    />
                    <span
                      className={cn(
                        'grid size-4 place-items-center rounded-[4px] border transition-colors',
                        shown ? 'border-transparent' : 'border-line',
                      )}
                      style={shown ? { background: venueColor(l.venue) } : undefined}
                    >
                      {shown && <Glyph name="check" className="size-3 text-canvas" />}
                    </span>
                  </button>
                )}
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}

/** the market on every venue that lists it: each touch, its depth at the touch, funding, and where they cross */
export function VenuesWidget() {
  const { pair, leg, quotes, funding, pick } = useTerminal()
  const cross = pair === undefined ? undefined : crossOf(pair, quotes)
  if (pair === undefined) return <Empty title="Pick a market" />
  // spot pays no funding: its column goes with it
  const perp = pair.market === 'perp'
  const cols = perp
    ? 'grid-cols-[minmax(84px,1.1fr)_1fr_1fr_52px_64px_48px]'
    : 'grid-cols-[minmax(84px,1.1fr)_1fr_1fr_52px_48px]'
  const now = Date.now()
  const bestBid = Math.max(...pair.legs.map((l) => touchOf(quotes.get(l.id), l)?.bid ?? -Infinity))
  const bestAsk = Math.min(...pair.legs.map((l) => touchOf(quotes.get(l.id), l)?.ask ?? Infinity))
  return (
    <div className="flex h-full min-h-0 flex-col">
      {cross !== undefined && (
        <div className="flex shrink-0 items-center gap-3 border-b border-line px-3 py-2 text-xs">
          <span className="flex items-center gap-1">
            <span className="text-up">Buy</span>
            <ExchangeIcon venue={cross.buy.leg.venue} className="size-3" />
          </span>
          <Glyph name="arrow" className="size-3 text-fg-dim" />
          <span className="flex items-center gap-1">
            <span className="text-down">Sell</span>
            <ExchangeIcon venue={cross.sell.leg.venue} className="size-3" />
          </span>
          <span className={cn('mono ml-auto font-semibold', cross.bp > 0 ? 'text-up' : 'text-fg-mid')}>
            {cross.bp > 0 ? '+' : ''}
            {bp(cross.bp, 2)}
          </span>
        </div>
      )}
      <div className={cn('col-head grid shrink-0 gap-2 px-3 pt-2 pb-1', cols)}>
        <span>Venue</span>
        <span className="text-right">Bid</span>
        <span className="text-right">Ask</span>
        <span className="text-right">Spread</span>
        {perp && <span className="text-right">Funding</span>}
        <span className="text-right">Age</span>
      </div>
      <div className="min-h-0 flex-1 overflow-y-auto [scrollbar-width:thin]">
        {pair.legs.map((l) => {
          const t = touchOf(quotes.get(l.id), l)
          const f = n(funding.get(l.id)?.rate)
          const on = l.venue === leg?.venue
          return (
            <button
              key={l.venue}
              type="button"
              onClick={() => pick({ venue: l.venue })}
              className={cn(
                'grid h-11 w-full cursor-pointer items-center gap-2 px-3 text-left text-xs transition-colors',
                cols,
                on ? 'bg-raised' : 'hover:bg-raised/50',
              )}
            >
              <span className="flex min-w-0 items-center gap-1.5">
                <span className="h-4 w-0.5 rounded-full" style={{ background: venueColor(l.venue) }} />
                <ExchangeIcon venue={l.venue} className="size-3.5" />
                <span className="truncate text-fg">{venueLabel(l.venue)}</span>
              </span>
              <span className="flex flex-col items-end">
                <span className={cn('mono', t?.bid === bestBid ? 'font-semibold text-up' : 'text-up/80')}>
                  <Flash value={t?.bid}>{px(t?.bid, l.places)}</Flash>
                </span>
                <span className="mono text-[10px] text-fg-dim">{qty(t?.bidSize)}</span>
              </span>
              <span className="flex flex-col items-end">
                <span className={cn('mono', t?.ask === bestAsk ? 'font-semibold text-down' : 'text-down/80')}>
                  <Flash value={t?.ask}>{px(t?.ask, l.places)}</Flash>
                </span>
                <span className="mono text-[10px] text-fg-dim">{qty(t?.askSize)}</span>
              </span>
              <span className="mono text-right text-fg-mid">{spreadBp(t)?.toFixed(2) ?? '—'}</span>
              {perp && (
                <span
                  className={cn(
                    'mono text-right',
                    f === undefined ? 'text-fg-dim' : f >= 0 ? 'text-fg-mid' : 'text-down',
                  )}
                >
                  {f === undefined ? '—' : `${(f * 100).toFixed(4)}%`}
                </span>
              )}
              <span className="mono text-right text-fg-dim">
                {t === undefined || !(t.ts > 0) ? '—' : `${Math.max(0, Math.round((now - t.ts) / 100) / 10)}s`}
              </span>
            </button>
          )
        })}
      </div>
    </div>
  )
}
