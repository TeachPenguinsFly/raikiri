import { useEffect, useRef, useState } from 'react'
import { ExchangeIcon } from '@/components/shared/ExchangeIcon'
import { Icon } from '@/components/shared/Icon'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { cn } from '@/lib/utils'
import type { MockListing } from '@/mock/data'

type Side = 'buy' | 'sell'
type Kind = 'limit' | 'market'

const num = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))

/** Where an order is written.
 *
 *  Docked beside the board rather than behind a button: a ticket you have to
 *  open is a ticket you fill in after the price you wanted has gone.
 *
 *  The venue is chosen INSIDE the ticket as well as on the board, and both
 *  write the same state — a ticket whose venue can only be changed somewhere
 *  else is a ticket you send to the wrong exchange. */
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
  const [price, setPrice] = useState('')
  const [qty, setQty] = useState('')
  const [sent, setSent] = useState<string | null>(null)

  const listing = listings.find((l) => l.venue === venue) ?? null

  // The limit price follows the side's own touch until a person types over it.
  // Prefilling from the mid instead would put every order on the wrong side of
  // the spread by half of it.
  const touch = listing === null ? null : side === 'buy' ? listing.bid : listing.ask
  useEffect(() => {
    if (touch !== null) setPrice(num(touch))
  }, [listing?.venue, listing?.exchangeSymbol, side, touch])

  const notional = Number(price) * Number(qty)
  const armed = listing !== null && Number(qty) > 0 && (kind === 'market' || Number(price) > 0)

  if (listing === null) {
    return (
      <section className="panel flex flex-col overflow-visible">
        <PanelHeader title="Ticket" />
        <div className="px-3 py-8 text-center text-2xs text-fg-mid">
          Pick a market to write an order against it.
        </div>
      </section>
    )
  }

  return (
    <section className="panel flex flex-col overflow-visible">
      <PanelHeader
        title="Ticket"
        right={<span className="micro-label font-mono">{listing.exchangeSymbol}</span>}
      />

      <div className="flex flex-col gap-2.5 p-3">
        <VenuePicker listings={listings} chosen={listing} onChoose={onVenue} />

        <div className="grid grid-cols-2 gap-1.5">
          {(['buy', 'sell'] as const).map((s) => (
            <button
              key={s}
              type="button"
              onClick={() => setSide(s)}
              className={cn(
                'h-8 rounded-[4px] text-2xs font-bold uppercase tracking-[0.06em] transition-colors',
                side === s
                  ? s === 'buy'
                    ? 'bg-up/20 text-up'
                    : 'bg-down/20 text-down'
                  : 'bg-white/4 text-fg-mid hover:bg-white/8 hover:text-fg',
              )}
            >
              {s}
            </button>
          ))}
        </div>

        <div className="flex gap-1.5">
          {(['limit', 'market'] as const).map((k) => (
            <button
              key={k}
              type="button"
              onClick={() => setKind(k)}
              className={cn(
                'h-6 flex-1 rounded-[3px] text-3xs font-semibold uppercase tracking-[0.06em] transition-colors',
                kind === k ? 'bg-white/10 text-fg' : 'text-fg-mid hover:text-fg',
              )}
            >
              {k}
            </button>
          ))}
        </div>

        <Field
          label="Price"
          suffix="USDT"
          value={kind === 'market' ? '' : price}
          placeholder={kind === 'market' ? 'at the touch' : ''}
          disabled={kind === 'market'}
          onChange={setPrice}
        />
        <Field
          label="Quantity"
          suffix={listing.exchangeSymbol.replace(/USDT.*|_.*|-.*/, '')}
          value={qty}
          onChange={setQty}
        />

        <div className="flex items-baseline justify-between px-0.5 text-3xs">
          <span className="text-fg-mid">Notional</span>
          <span className="num text-fg">
            {Number.isFinite(notional) && notional > 0 ? `${notional.toFixed(2)} USDT` : '—'}
          </span>
        </div>

        <button
          type="button"
          disabled={!armed}
          onClick={() => setSent(`${side} ${qty} ${listing.exchangeSymbol} on ${listing.venue}`)}
          className={cn(
            'h-9 rounded-[4px] text-2xs font-bold uppercase tracking-[0.08em] transition-[transform,background-color] duration-150',
            'active:scale-[0.985] disabled:cursor-not-allowed disabled:opacity-40',
            side === 'buy' ? 'bg-up/85 text-canvas hover:bg-up' : 'bg-down/85 text-canvas hover:bg-down',
          )}
        >
          {side} {kind}
        </button>

        {/* The write path does not exist yet, and the ticket says so rather than
            pretending an order went somewhere. */}
        <p className="text-3xs text-fg-dim">{sent ?? 'Reads only — nothing is sent yet.'}</p>
      </div>
    </section>
  )
}

/** The exchange the order goes to.
 *
 *  A native select cannot draw the venue's own mark, and the mark is how every
 *  other row in the terminal is identified — so this is a button and a panel.
 *  Each row carries the side's touch, because "which venue" and "at what price"
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
          'flex h-9 w-full items-center gap-2 rounded-[4px] border bg-canvas/60 px-2.5 transition-colors',
          open ? 'border-accent/70' : 'border-line hover:border-line2',
        )}
      >
        <ExchangeIcon venue={chosen.venue} className="size-[17px]" />
        <span className="text-2xs font-semibold text-fg">{chosen.venue}</span>
        <span className="num ml-auto text-3xs text-fg-mid">{num(chosen.bid)}</span>
        <Icon.Chevron
          className={cn('size-3.5 text-fg-mid transition-transform', open && 'rotate-180')}
        />
      </button>

      {open && (
        <div className="panel absolute inset-x-0 top-10 z-20 max-h-64 overflow-auto py-1 shadow-[0_16px_40px_-12px_rgba(0,0,0,0.85)]">
          {listings.map((l) => (
            <button
              key={l.venue}
              type="button"
              onClick={() => {
                onChoose(l.venue)
                setOpen(false)
              }}
              className={cn(
                'flex h-8 w-full items-center gap-2 px-2.5 text-left transition-colors',
                l.venue === chosen.venue ? 'bg-accent/10' : 'hover:bg-hover',
              )}
            >
              <ExchangeIcon venue={l.venue} className="size-[15px]" />
              <span
                className={cn(
                  'text-2xs',
                  l.venue === chosen.venue ? 'font-semibold text-fg' : 'text-fg-mid',
                )}
              >
                {l.venue}
              </span>
              <span className="num ml-auto text-3xs text-up">{num(l.bid)}</span>
              <span className="num text-3xs text-down">{num(l.ask)}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

function Field({
  label,
  suffix,
  value,
  placeholder,
  disabled,
  onChange,
}: {
  label: string
  suffix: string
  value: string
  placeholder?: string
  disabled?: boolean
  onChange: (v: string) => void
}) {
  return (
    <label
      className={cn(
        'flex h-9 items-center gap-2 rounded-[4px] border border-line bg-canvas/60 px-2.5',
        'transition-colors focus-within:border-accent/70',
        disabled === true && 'opacity-45',
      )}
    >
      <span className="w-14 shrink-0 text-3xs font-semibold text-fg-mid">{label}</span>
      <input
        inputMode="decimal"
        value={value}
        placeholder={placeholder}
        disabled={disabled}
        onChange={(e) => onChange(e.target.value)}
        className="num min-w-0 flex-1 bg-transparent text-right text-[13px] text-fg outline-none placeholder:text-fg-dim"
      />
      <span className="shrink-0 text-3xs text-fg-mid">{suffix}</span>
    </label>
  )
}
