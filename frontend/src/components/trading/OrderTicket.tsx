import { useEffect, useRef, useState } from 'react'
import { ExchangeIcon } from '@/components/shared/ExchangeIcon'
import { Icon } from '@/components/shared/Icon'
import { cn } from '@/lib/utils'
import { BALANCES, type MockListing } from '@/mock/data'

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
  const [kind, setKind] = useState<Kind>('limit')
  const [limit, setLimit] = useState('')
  const [qty, setQty] = useState('')
  const [sent, setSent] = useState<string | null>(null)

  const listing = listings.find((l) => l.venue === venue) ?? null

  // The limit price starts at the bid and is moved by pressing a touch, not by
  // a side toggle. Prefilling from the mid instead would put every order on the
  // wrong side of the spread by half of it, and there is no side to prefill
  // from until a person presses Buy or Sell.
  useEffect(() => {
    if (listing !== null) setLimit(price(listing.bid))
  }, [listing?.venue, listing?.exchangeSymbol])

  if (listing === null) {
    return (
      <section className="panel flex flex-col overflow-visible">
        <div className="flex h-7 items-center px-3">
          <span className="panel-title">Ticket</span>
        </div>
        <p className="px-3 pb-3 text-xs text-fg-dim">Pick a market to write an order against.</p>
      </section>
    )
  }

  const base = listing.exchangeSymbol.replace(/USDT.*|_.*|-.*/, '')
  const working = kind === 'market' ? listing.ask : Number(limit)
  const notional = working * Number(qty)
  const armed = Number(qty) > 0 && (kind === 'market' || Number(limit) > 0)

  // Sizing buttons spend the venue's own free figure. NULL IS NOT FLAT: where
  // a venue publishes no per-asset available balance the row is refused rather
  // than sized off a total that is already committed as margin.
  const free =
    BALANCES.find((b) => b.venue === listing.venue && b.asset === 'USDT')?.available ?? null

  return (
    <section className="panel flex flex-col overflow-visible">
      <div className="flex h-7 items-center justify-between px-3">
        <span className="panel-title">Ticket</span>
        <span className="text-2xs text-fg-dim">{listing.exchangeSymbol}</span>
      </div>

      <div className="flex flex-col gap-2 px-3 pb-3">
        <VenuePicker listings={listings} chosen={listing} onChoose={onVenue} />

        {/* The two touches, as buttons. Pressing one puts it in the price
            field — which is the only thing a trader wants from a quoted price
            while a ticket is open. */}
        <div className="flex items-stretch gap-1">
          <button
            type="button"
            onClick={() => setLimit(price(listing.bid))}
            className="num flex h-8 flex-1 items-center justify-center rounded-sm bg-up/[0.10] text-sm text-up transition-colors duration-150 hover:bg-up/[0.16]"
          >
            {price(listing.bid)}
          </button>
          <span className="num flex w-14 shrink-0 items-center justify-center text-2xs text-fg-dim">
            {(((listing.ask - listing.bid) / listing.bid) * 10_000).toFixed(1)} bp
          </span>
          <button
            type="button"
            onClick={() => setLimit(price(listing.ask))}
            className="num flex h-8 flex-1 items-center justify-center rounded-sm bg-down/[0.10] text-sm text-down transition-colors duration-150 hover:bg-down/[0.16]"
          >
            {price(listing.ask)}
          </button>
        </div>

        <div className="flex gap-4">
          {(['limit', 'market'] as const).map((k) => (
            <button
              key={k}
              type="button"
              onClick={() => setKind(k)}
              className={cn(
                'relative h-6 text-xs capitalize transition-colors duration-150',
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
          placeholder={kind === 'market' ? price(listing.ask) : ''}
          disabled={kind === 'market'}
          onChange={setLimit}
        />
        <Field label="Quantity" unit={base} value={qty} onChange={setQty} />

        <div className="flex gap-1" title={free === null ? 'this venue publishes no free balance' : undefined}>
          {[0.25, 0.5, 1].map((f) => (
            <button
              key={f}
              type="button"
              disabled={free === null || working <= 0}
              onClick={() => setQty(((free as number) * f / working).toFixed(4))}
              className="h-6 flex-1 rounded-sm bg-raised text-2xs text-fg-mid transition-colors duration-150 hover:bg-hover hover:text-fg disabled:cursor-not-allowed disabled:text-fg-dim disabled:hover:bg-raised"
            >
              {f === 1 ? 'Max' : `${f * 100}%`}
            </button>
          ))}
          <button
            type="button"
            onClick={() => setQty('')}
            className="h-6 flex-1 rounded-sm bg-raised text-2xs text-fg-dim transition-colors duration-150 hover:bg-hover hover:text-fg-mid"
          >
            Clear
          </button>
        </div>

        {/* Two blocks, not a toggle and a button. The side is the press, so
            there is no state to set and then confirm — and each block states
            what it would do, which is the sentence a ticket exists to make. */}
        <div className="grid grid-cols-2 gap-1.5 pt-0.5">
          {(['buy', 'sell'] as const).map((s) => (
            <button
              key={s}
              type="button"
              disabled={!armed}
              onClick={() => setSent(`${s} ${qty} ${base} on ${listing.venue}`)}
              className={cn(
                'flex flex-col items-center gap-0.5 rounded-sm py-2 transition-colors duration-150',
                'disabled:cursor-not-allowed disabled:bg-raised disabled:text-fg-dim',
                armed && (s === 'buy'
                  ? 'bg-up/[0.14] text-up hover:bg-up/[0.22]'
                  : 'bg-down/[0.14] text-down hover:bg-down/[0.22]'),
              )}
            >
              <span className="text-sm font-medium capitalize">
                {s} {base}
              </span>
              <span className={cn('num text-2xs', armed ? 'opacity-70' : 'text-fg-dim')}>
                {Number.isFinite(notional) && notional > 0
                  ? `${grouped(notional)} USDT`
                  : `0 ${base}`}
              </span>
            </button>
          ))}
        </div>

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
          'flex h-8 w-full items-center gap-2 rounded-sm px-2 transition-colors duration-150',
          open ? 'bg-active' : 'bg-raised hover:bg-hover',
        )}
      >
        <ExchangeIcon venue={chosen.venue} className="size-[18px]" />
        <span className="text-sm text-fg">{chosen.venue}</span>
        <span className="num ml-auto text-xs text-fg-dim">{price(chosen.bid)}</span>
        <Icon.Caret
          className={cn('size-3.5 text-fg-dim transition-transform duration-150', open && 'rotate-180')}
          strokeWidth={1.75}
        />
      </button>

      {open && (
        <div className="absolute inset-x-0 top-9 z-30 max-h-64 overflow-auto rounded-md bg-raised p-1 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.8)]">
          {listings.map((l) => (
            <button
              key={l.venue}
              type="button"
              onClick={() => {
                onChoose(l.venue)
                setOpen(false)
              }}
              className={cn(
                'flex h-7 w-full items-center gap-2 rounded-sm px-1.5 text-left transition-colors duration-150',
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
    <label className={cn('flex flex-col gap-1', disabled === true && 'opacity-45')}>
      <span className="text-2xs text-fg-dim">{label}</span>
      <span
        className={cn(
          'flex h-8 items-center gap-2 rounded-sm bg-raised px-2',
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
          className="num min-w-0 flex-1 bg-transparent text-sm text-fg outline-none placeholder:text-fg-dim"
        />
        <span className="shrink-0 text-2xs text-fg-dim">{unit}</span>
      </span>
    </label>
  )
}
