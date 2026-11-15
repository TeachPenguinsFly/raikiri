import { useEffect, useMemo, useState, type ReactNode } from 'react'
import { cn, ExchangeIcon, venueLabel } from '@raikiri/ui'
import { compact, Corners, Empty, Seg, Stepper, Ticker, walk, type Fill } from '@raikiri/ui/v3'
import { bp, px, qty, takerBp, touchOf } from '../lib/market'
import { useBook } from './book'
import { useTerminal } from './state'

// a ticket that prices an order against the books it would meet, on the chosen venue and on every other one: what it
// pays on average, the fee, how far it walks. There is no order route behind it, so it sends nothing

type Action = 'buy' | 'sell'
type Kind = 'market' | 'limit'
type Unit = 'usd' | 'base'

const k_sizes = [1_000, 10_000, 50_000, 250_000]

function Line({ label, children, tone }: { label: ReactNode; children: ReactNode; tone?: string }) {
  return (
    <div className="flex items-center justify-between py-[3px] text-xs">
      <span className="text-fg-dim">{label}</span>
      <span className={cn('mono', tone ?? 'text-fg')}>{children}</span>
    </div>
  )
}

interface Cost {
  venue: string
  avg: number | undefined
  /** the dollars that change hands, fee in */
  total: number
  /** what one coin costs or returns, fee in, so venues compare on it */
  each: number | undefined
  short: boolean
}

function VenueRow({ c, on, best, onPick }: { c: Cost; on: boolean; best: boolean; onPick: () => void }) {
  return (
    <button
      type="button"
      onClick={onPick}
      className={cn(
        'grid h-7 cursor-pointer grid-cols-[1fr_1fr_1fr] items-center gap-2 rounded-sm px-2 text-2xs transition-colors',
        on ? 'bg-raised' : 'hover:bg-raised/50',
      )}
    >
      <span className="flex items-center gap-1.5 text-fg-mid">
        <ExchangeIcon venue={c.venue} className="size-3" />
        {venueLabel(c.venue)}
      </span>
      <span className="mono text-right text-fg">{c.avg === undefined ? '—' : px(c.avg, 2)}</span>
      <span className={cn('mono text-right', best ? 'text-up' : c.short ? 'text-amber' : 'text-fg-mid')}>
        {c.total > 0 ? compact(c.total, true) : '—'}
      </span>
    </button>
  )
}

export function TicketWidget() {
  const { pair, leg, pick, draft, quotes } = useTerminal()
  const [action, setAction] = useState<Action>('buy')
  const [kind, setKind] = useState<Kind>('market')
  const [unit, setUnit] = useState<Unit>('usd')
  const [amount, setAmount] = useState('10000')
  const [limit, setLimit] = useState(0)
  const [slip, setSlip] = useState(10)
  const venue = leg?.venue ?? ''
  const book = useBook(venue)
  const touch = leg === undefined ? undefined : touchOf(quotes.get(leg.id), leg)

  // a click on a level lands here as a limit at that price
  useEffect(() => {
    if (draft === undefined) return
    setAction(draft.action)
    if (draft.price !== undefined) {
      setKind('limit')
      setLimit(draft.price)
    }
  }, [draft])

  const levels = action === 'buy' ? book.asks : book.bids
  const best = levels[0]?.price
  useEffect(() => {
    if (kind === 'limit' && limit === 0 && best !== undefined) setLimit(best)
  }, [kind, limit, best])

  const cap =
    kind === 'limit' ? limit : best === undefined ? undefined : best * (1 + ((action === 'buy' ? 1 : -1) * slip) / 1e4)
  const want = Number(amount) || 0
  const fill: Fill = useMemo(
    () => walk(levels, want, unit === 'usd' ? 'usd' : 'shares', cap, action),
    [levels, want, unit, cap, action],
  )
  const avg = fill.shares > 0 ? fill.usd / fill.shares : undefined
  const feeBp = takerBp(venue)
  const fee = (fill.usd * feeBp) / 1e4
  const impact = avg !== undefined && best !== undefined ? (Math.abs(avg - best) / best) * 1e4 : undefined
  const vsMid =
    avg !== undefined && touch !== undefined
      ? (((action === 'buy' ? 1 : -1) * (avg - touch.mid)) / touch.mid) * 1e4
      : undefined
  const rest =
    kind === 'limit' && fill.short && limit > 0 ? (unit === 'usd' ? (want - fill.usd) / limit : want - fill.shares) : 0

  // the venue that fills the same order for the least, fee in
  const legs = pair?.legs ?? []
  const costs = useVenueCosts(
    legs.map((l) => l.venue),
    action,
    want,
    unit,
    slip,
  )
  const cheapest = costs.reduce<Cost | undefined>(
    (a, c) =>
      c.each === undefined || c.short
        ? a
        : a?.each === undefined || (action === 'buy' ? c.each < a.each : c.each > a.each)
          ? c
          : a,
    undefined,
  )?.venue

  if (pair === undefined || leg === undefined) return <Empty title="Pick a market" />

  const field = (u: Unit) => (u === 'usd' ? fill.usd : fill.shares)
  const tone = (v: Action) =>
    v === 'buy'
      ? 'bg-up/20 shadow-[0_0_0_1px_rgb(47_211_166/0.35)_inset]'
      : 'bg-down/20 shadow-[0_0_0_1px_rgb(242_85_125/0.35)_inset]'

  return (
    <div className="flex h-full min-h-0 flex-col gap-2.5 overflow-y-auto p-2.5 [scrollbar-width:thin]">
      <div className="flex items-center gap-2">
        <Seg
          size="sm"
          className="flex-1"
          value={action}
          onChange={setAction}
          tone={tone}
          options={[
            ['buy', 'Buy'],
            ['sell', 'Sell'],
          ]}
        />
        <Seg
          size="sm"
          value={kind}
          onChange={setKind}
          options={[
            ['market', 'Market'],
            ['limit', 'Limit'],
          ]}
        />
      </div>

      <div className="grid grid-cols-2 gap-1.5">
        {(['buy', 'sell'] as const).map((a) => {
          const on = action === a
          const p = a === 'buy' ? touch?.ask : touch?.bid
          return (
            <button
              key={a}
              type="button"
              onClick={() => setAction(a)}
              className={cn(
                'flex h-12 cursor-pointer flex-col items-start justify-center gap-0.5 rounded-md px-3 text-left transition-all duration-200',
                on
                  ? a === 'buy'
                    ? 'bg-up/15 text-up shadow-[0_0_0_1px_rgb(47_211_166/0.45)_inset,0_8px_24px_-12px_rgb(47_211_166/0.6)]'
                    : 'bg-down/15 text-down shadow-[0_0_0_1px_rgb(242_85_125/0.45)_inset,0_8px_24px_-12px_rgb(242_85_125/0.6)]'
                  : 'bg-raised/60 text-fg-mid hover:bg-hover',
              )}
            >
              <span className="text-xs font-medium">{a === 'buy' ? 'Ask' : 'Bid'}</span>
              <span className="mono text-sm font-semibold">{px(p, leg.places)}</span>
            </button>
          )
        })}
      </div>

      {legs.length > 1 && (
        <Seg
          size="xs"
          value={venue}
          onChange={(v) => pick({ venue: v })}
          options={legs.map(
            (l) =>
              [
                l.venue,
                <span key={l.venue} className="flex items-center gap-1">
                  <ExchangeIcon venue={l.venue} className="size-3" />
                  {venueLabel(l.venue)}
                </span>,
              ] as const,
          )}
        />
      )}

      {kind === 'limit' && (
        <div className="flex items-center justify-between">
          <span className="text-xs text-fg-dim">Limit price</span>
          <Stepper value={limit} step={leg.tick} min={leg.tick} max={1e12} onChange={setLimit} />
        </div>
      )}

      <div className="flex gap-1.5">
        {(['usd', 'base'] as const).map((u) => (
          <label
            key={u}
            className={cn(
              'flex h-10 min-w-0 flex-1 items-center gap-1.5 rounded-md bg-canvas/60 px-2.5 transition-shadow',
              unit === u ? 'shadow-[0_0_0_1px_rgb(var(--v3-glow)/0.55)]' : 'shadow-[0_0_0_1px_var(--color-line)]',
            )}
          >
            {u === 'usd' && <span className="text-xs text-fg-dim">$</span>}
            <input
              inputMode="decimal"
              aria-label={u === 'usd' ? 'dollars' : pair.base}
              value={unit === u ? amount : field(u) > 0 ? String(Number(field(u).toFixed(u === 'usd' ? 2 : 6))) : ''}
              onFocus={() => {
                if (unit === u) return
                setAmount(field(u) > 0 ? String(Number(field(u).toFixed(u === 'usd' ? 2 : 6))) : '')
                setUnit(u)
              }}
              onChange={(e) => setAmount(e.target.value.replace(/[^\d.]/g, ''))}
              placeholder="0"
              className="mono w-0 min-w-0 flex-1 bg-transparent text-sm text-fg outline-none placeholder:text-fg-dim"
            />
            <span className="text-[10px] text-fg-dim uppercase">{u === 'usd' ? 'usd' : pair.base}</span>
          </label>
        ))}
      </div>

      <div className="grid grid-cols-4 gap-1">
        {k_sizes.map((k) => (
          <button
            key={k}
            type="button"
            onClick={() => {
              setUnit('usd')
              setAmount(String(k))
            }}
            className={cn(
              'h-7 cursor-pointer rounded-md text-2xs transition-all hover:bg-hover hover:text-fg active:scale-95',
              unit === 'usd' && want === k ? 'bg-raised text-fg' : 'bg-raised/60 text-fg-mid',
            )}
          >
            {compact(k, true).replace('.00', '').replace('.0K', 'K')}
          </button>
        ))}
      </div>

      {kind === 'market' && (
        <div className="flex items-center justify-between gap-2">
          <span className="text-xs text-fg-dim">Max slip</span>
          <Stepper value={slip} step={1} min={1} max={200} suffix=" bp" onChange={setSlip} />
        </div>
      )}

      <div className="relative rounded-md bg-canvas/50 px-3 py-2">
        <Corners color="rgb(255 255 255 / 0.18)" />
        <Line label="Avg. price">
          <Ticker value={avg} format={(v) => px(v, leg.places + 1)} />
        </Line>
        <Line label={pair.base}>
          <Ticker value={fill.shares} format={qty} />
        </Line>
        <Line label={action === 'buy' ? 'Cost' : 'Proceeds'}>
          <Ticker value={fill.usd} format={(v) => compact(v, true)} />
        </Line>
        <Line label="Taker fee" tone="text-fg-mid">
          {`${compact(fee, true)} · ${feeBp} bp`}
        </Line>
        <Line label="Price impact" tone={impact !== undefined && impact > 5 ? 'text-amber' : 'text-fg-mid'}>
          {impact === undefined ? '—' : `${bp(impact, 2)} · ${fill.levels} lvl`}
        </Line>
        <Line label="Off mid" tone="text-fg-mid">
          {bp(vsMid, 2)}
        </Line>
        {fill.short && kind === 'market' && (
          <Line label="Unfilled" tone="text-amber">
            {unit === 'usd' ? compact(want - fill.usd, true) : `${qty(want - fill.shares)} ${pair.base}`}
          </Line>
        )}
        {rest > 0 && <Line label={`Rests at ${px(limit, leg.places)}`}>{`${qty(rest)} ${pair.base}`}</Line>}
      </div>

      {legs.length > 1 && (
        <div className="flex flex-col gap-0.5">
          <div className="col-head grid grid-cols-[1fr_1fr_1fr] gap-2 px-2 pb-0.5">
            <span>Venue</span>
            <span className="text-right">Avg.</span>
            <span className="text-right">{action === 'buy' ? 'Cost' : 'Proceeds'}</span>
          </div>
          {costs.map((c) => (
            <VenueRow
              key={c.venue}
              c={c}
              on={c.venue === venue}
              best={c.venue === cheapest}
              onPick={() => pick({ venue: c.venue })}
            />
          ))}
        </div>
      )}

      <div className="mt-auto flex flex-col gap-1.5">
        <button
          type="button"
          disabled
          className={cn(
            'h-10 cursor-not-allowed rounded-md text-sm font-semibold transition-all',
            action === 'buy' ? 'bg-up/15 text-up/80' : 'bg-down/15 text-down/80',
          )}
        >
          {action === 'buy' ? 'Buy' : 'Sell'} {pair.base} on {venueLabel(venue)} · preview
        </button>
      </div>
    </div>
  )
}

/** the same order walked down every venue's book */
function useVenueCosts(venues: string[], action: Action, want: number, unit: Unit, slip: number): Cost[] {
  const { pair, books } = useTerminal()
  const sign = action === 'buy' ? 1 : -1
  return useMemo(
    () =>
      venues.map((venue) => {
        const leg = pair?.legs.find((l) => l.venue === venue)
        const side = leg === undefined ? undefined : books.get(leg.id)?.[action === 'buy' ? 'asks' : 'bids']
        const levels = (side ?? []).flatMap((l) => {
          const price = Number(l.price)
          const size = Number(l.size) * (leg?.unit ?? 1)
          return Number.isFinite(price) && size > 0 ? [{ price, size, parts: {} }] : []
        })
        const top = levels[0]?.price
        const cap = top === undefined ? undefined : top * (1 + (sign * slip) / 1e4)
        const f = walk(levels, want, unit === 'usd' ? 'usd' : 'shares', cap, action)
        const fee = 1 + (sign * takerBp(venue)) / 1e4
        const avg = f.shares > 0 ? f.usd / f.shares : undefined
        return { venue, avg, total: f.usd * fee, each: avg === undefined ? undefined : avg * fee, short: f.short }
      }),
    [venues.join(','), pair, books, action, want, unit, slip, sign],
  )
}
