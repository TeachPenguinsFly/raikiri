import { useEffect, useRef, useState } from 'react'
import { ExchangeIcon } from '@/components/shared/ExchangeIcon'
import { Icon } from '@/components/shared/Icon'
import { cn } from '@/lib/utils'
import type { MockListing } from '@/mock/data'

type Side = 'buy' | 'sell'
type Kind = 'limit' | 'market'

const price = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))
const grouped = (v: number) =>
  v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })

/** Where an order is written.
 *
 *  Docked beside the board rather than behind a button: a ticket you have to
 *  open is a ticket you fill in after the price you wanted has gone.
 *
 *  The side is chosen once, at the top, and after that the panel does not
 *  change colour. Green belongs to the selected Buy and to the Buy action, and
 *  to nothing else — a panel that turns green makes the direction ambient
 *  instead of deliberate, which is the wrong property for the one control on
 *  the screen that spends money. */
export function OrderTicket({
  listings,
  venue,
  onVenue,
}: {
  listings: MockListing[]
  venue: string | null
  onVenue: (venue: string) => void
}) {
  const [side, setSide] = useState<Side>('buy')
  const [kind, setKind] = useState<Kind>('limit')
  const [limit, setLimit] = useState('')
  const [qty, setQty] = useState('')
  const [sent, setSent] = useState<string | null>(null)

  const listing = listings.find((l) => l.venue === venue) ?? null

  // The limit price follows the side's own touch until a person types over it.
  // Prefilling from the mid instead would put every order on the wrong side of
  // the spread by half of it.
  const touch = listing === null ? null : side === 'buy' ? listing.bid : listing.ask
  useEffect(() => {
    if (touch !== null) setLimit(price(touch))
  }, [listing?.venue, listing?.exchangeSymbol, side, touch])

  if (listing === null) {
    return (
      <section className="panel flex flex-col overflow-visible">
        <div className="panel-title px-3 pt-3">Ticket</div>
        <p className="px-3 py-10 text-center text-xs text-fg-dim">
          Pick a market to write an order against.
        </p>
      </section>
    )
  }

  const base = listing.exchangeSymbol.replace(/USDT.*|_.*|-.*/, '')
  const working = kind === 'market' ? (touch ?? 0) : Number(limit)
  const notional = working * Number(qty)
  const armed = Number(qty) > 0 && (kind === 'market' || Number(limit) > 0)

  return (
    <section className="panel flex flex-col overflow-visible">
      <div className="flex h-11 items-center justify-between px-3">
        <span className="panel-title">Ticket</span>
        <span className="font-mono text-xs text-fg-dim">{listing.exchangeSymbol}</span>
      </div>

      <div className="flex flex-col gap-3 px-3 pb-3">
        <VenuePicker listings={listings} chosen={listing} onChoose={onVenue} />

        {/* One segmented control, one thumb. Two buttons that each light up
            read as two independent toggles; a track with a moving thumb reads
            as one choice with two positions, which is what it is. */}
        <div className="relative grid h-9 grid-cols-2 rounded-md bg-raised p-0.5">
          <span
            className={cn(
              'absolute inset-y-0.5 w-[calc(50%-2px)] rounded-sm transition-transform duration-150 ease-out',
              side === 'buy' ? 'translate-x-0 bg-up/[0.16]' : 'translate-x-[calc(100%+0px)] bg-down/[0.16]',
            )}
          />
          {(['buy', 'sell'] as const).map((s) => (
            <button
              key={s}
              type="button"
              onClick={() => setSide(s)}
              className={cn(
                'relative z-10 text-sm font-medium capitalize transition-colors duration-150',
                side === s ? (s === 'buy' ? 'text-up' : 'text-down') : 'text-fg-mid hover:text-fg',
              )}
            >
              {s}
            </button>
          ))}
        </div>

        <div className="flex gap-4">
          {(['limit', 'market'] as const).map((k) => (
            <button
              key={k}
              type="button"
              onClick={() => setKind(k)}
              className={cn(
                'relative h-7 text-xs capitalize transition-colors duration-150',
                kind === k
                  ? 'font-medium text-fg after:absolute after:inset-x-0 after:-bottom-px after:h-px after:bg-accent'
                  : 'text-fg-dim hover:text-fg-mid',
              )}
            >
              {k}
            </button>
          ))}
        </div>

        <Field
          label="Price"
          unit="USDT"
          value={kind === 'market' ? '' : limit}
          placeholder={kind === 'market' ? price(touch ?? 0) : ''}
          disabled={kind === 'market'}
          onChange={setLimit}
        />
        <Field label="Quantity" unit={base} value={qty} onChange={setQty} />

        <div className="flex items-baseline justify-between">
          <span className="text-xs text-fg-dim">Notional</span>
          <span className="num text-sm text-fg-mid">
            {Number.isFinite(notional) && notional > 0 ? `≈ ${grouped(notional)} USDT` : '—'}
          </span>
        </div>

        <button
          type="button"
          disabled={!armed}
          onClick={() => setSent(`${side} ${qty} ${base} on ${listing.venue}`)}
          className={cn(
            'h-9 rounded-md text-sm font-semibold transition-all duration-150',
            'active:scale-[0.99] disabled:cursor-not-allowed disabled:bg-raised disabled:text-fg-dim',
            armed && (side === 'buy' ? 'bg-up text-canvas hover:brightness-110' : 'bg-down text-canvas hover:brightness-110'),
          )}
        >
          {side === 'buy' ? 'Buy' : 'Sell'} {base}
        </button>

        {/* The write path does not exist yet, and the ticket says so rather
            than pretending an order went somewhere. */}
        <p className="text-2xs text-fg-dim">{sent ?? 'Reads only — nothing is sent yet.'}</p>
      </div>
    </section>
  )
}

/** The exchange the order goes to.
 *
 *  A native select cannot draw the venue's own mark, and the mark is how every
 *  other row in the terminal is identified — so this is a button and a panel.
 *  Each row carries both touches, because "which venue" and "at what price"
 *  are the same question. */
function VenuePicker({
  listings,
  chosen,
  onChoose,
}: {
  listings: MockListing[]
  chosen: MockListing
  onChoose: (venue: string) => void
}) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const away = (e: PointerEvent) => {
      if (box.current?.contains(e.target as Node) !== true) setOpen(false)
    }
    const escape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    document.addEventListener('pointerdown', away)
    document.addEventListener('keydown', escape)
    return () => {
      document.removeEventListener('pointerdown', away)
      document.removeEventListener('keydown', escape)
    }
  }, [open])

  return (
    <div ref={box} className="relative">
      <button
        type="button"
        onClick={() => setOpen(!open)}
        className={cn(
          'flex h-9 w-full items-center gap-2 rounded-md px-2.5 transition-colors duration-150',
          open ? 'bg-active' : 'bg-raised hover:bg-hover',
        )}
      >
        <ExchangeIcon venue={chosen.venue} className="size-[18px]" />
        <span className="text-sm text-fg">{chosen.venue}</span>
        <span className="num ml-auto text-xs text-fg-dim">{price(chosen.bid)}</span>
        <Icon.Chevron
          className={cn('size-3.5 text-fg-dim transition-transform duration-150', open && 'rotate-180')}
          strokeWidth={1.75}
        />
      </button>

      {open && (
        <div className="absolute inset-x-0 top-10 z-30 max-h-64 overflow-auto rounded-lg bg-raised p-1 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.7)]">
          {listings.map((l) => (
            <button
              key={l.venue}
              type="button"
              onClick={() => {
                onChoose(l.venue)
                setOpen(false)
              }}
              className={cn(
                'flex h-8 w-full items-center gap-2 rounded-sm px-1.5 text-left transition-colors duration-150',
                l.venue === chosen.venue ? 'bg-active' : 'hover:bg-hover',
              )}
            >
              <ExchangeIcon venue={l.venue} className="size-4" />
              <span className={cn('text-sm', l.venue === chosen.venue ? 'text-fg' : 'text-fg-mid')}>
                {l.venue}
              </span>
              <span className="num ml-auto text-xs text-up">{price(l.bid)}</span>
              <span className="num w-[52px] text-right text-xs text-down">{price(l.ask)}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

/** Label above, value and unit inside.
 *
 *  The label sits outside the box rather than inside it: a label competing for
 *  the same 36px as the number you are typing is a label you read every time
 *  and a number you read around. */
function Field({
  label,
  unit,
  value,
  placeholder,
  disabled,
  onChange,
}: {
  label: string
  unit: string
  value: string
  placeholder?: string
  disabled?: boolean
  onChange: (v: string) => void
}) {
  return (
    <label className={cn('flex flex-col gap-1.5', disabled === true && 'opacity-45')}>
      <span className="text-xs text-fg-dim">{label}</span>
      <span
        className={cn(
          'flex h-9 items-center gap-2 rounded-md bg-raised px-2.5',
          'transition-shadow duration-150',
          'focus-within:shadow-[inset_0_0_0_1px_var(--color-accent)]',
        )}
      >
        <input
          inputMode="decimal"
          value={value}
          placeholder={placeholder}
          disabled={disabled}
          onChange={(e) => onChange(e.target.value)}
          className="num min-w-0 flex-1 bg-transparent text-base text-fg outline-none placeholder:text-fg-dim"
        />
        <span className="shrink-0 text-xs text-fg-dim">{unit}</span>
      </span>
    </label>
  )
}
