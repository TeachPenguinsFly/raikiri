import { useEffect, useState } from 'react'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { VenueTag } from '@/components/shared/VenueTag'
import { cn } from '@/lib/utils'

type Side = 'buy' | 'sell'
type Kind = 'limit' | 'market'

const num = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))

/** Where an order is written.
 *
 *  Docked beside the board rather than behind a button: a ticket you have to
 *  open is a ticket you fill in after the price you wanted has gone. */
export function OrderTicket({
  venue,
  symbol,
  bid,
  ask,
}: {
  venue: string | null
  symbol: string | null
  bid: number | null
  ask: number | null
}) {
  const [side, setSide] = useState<Side>('buy')
  const [kind, setKind] = useState<Kind>('limit')
  const [price, setPrice] = useState('')
  const [qty, setQty] = useState('')
  const [sent, setSent] = useState<string | null>(null)

  // The limit price follows the side's own touch until a person types over it.
  // Prefilling from the mid instead would put every order on the wrong side of
  // the spread by half of it.
  const touch = side === 'buy' ? bid : ask
  useEffect(() => {
    if (touch !== null) setPrice(num(touch))
  }, [venue, symbol, side, touch])

  const notional = Number(price) * Number(qty)
  const armed = venue !== null && Number(qty) > 0 && (kind === 'market' || Number(price) > 0)

  if (venue === null || symbol === null) {
    return (
      <section className="panel flex flex-col">
        <PanelHeader title="Ticket" />
        <div className="px-3 py-8 text-center text-2xs text-fg-dim">
          Pick a venue on the board to write an order against it.
        </div>
      </section>
    )
  }

  return (
    <section className="panel flex flex-col">
      <PanelHeader
        title={
          <>
            Ticket
            <VenueTag venue={venue} />
          </>
        }
        right={<span className="micro-label font-mono">{symbol}</span>}
      />

      <div className="flex flex-col gap-2.5 p-3">
        <div className="grid grid-cols-2 gap-1.5">
          {(['buy', 'sell'] as const).map((s) => (
            <button
              key={s}
              type="button"
              onClick={() => setSide(s)}
              className={cn(
                'h-8 rounded-lg text-2xs font-bold uppercase tracking-[0.06em] transition-colors',
                side === s
                  ? s === 'buy'
                    ? 'bg-up/20 text-up'
                    : 'bg-down/20 text-down'
                  : 'bg-white/4 text-fg-dim hover:bg-white/8 hover:text-fg-mid',
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
                'h-6 flex-1 rounded-md text-3xs font-semibold uppercase tracking-[0.06em] transition-colors',
                kind === k ? 'bg-white/10 text-fg' : 'text-fg-dim hover:text-fg-mid',
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
        <Field label="Quantity" suffix={symbol.replace(/USDT.*|_.*|-.*/, '')} value={qty} onChange={setQty} />

        <div className="flex items-baseline justify-between px-0.5 text-3xs">
          <span className="text-fg-dim">Notional</span>
          <span className="num text-fg-mid">
            {Number.isFinite(notional) && notional > 0 ? `${notional.toFixed(2)} USDT` : '—'}
          </span>
        </div>

        <button
          type="button"
          disabled={!armed}
          onClick={() => setSent(`${side} ${qty} ${symbol} on ${venue}`)}
          className={cn(
            'h-9 rounded-lg text-2xs font-bold uppercase tracking-[0.08em] transition-[transform,background-color] duration-150',
            'active:scale-[0.985] disabled:cursor-not-allowed disabled:opacity-40',
            side === 'buy' ? 'bg-up/85 text-canvas hover:bg-up' : 'bg-down/85 text-canvas hover:bg-down',
          )}
        >
          {side} {kind}
        </button>

        {/* The write path does not exist yet, and the ticket says so rather than
            pretending an order went somewhere. */}
        <p className="text-3xs leading-relaxed text-fg-dim">
          {sent ?? 'Nothing is sent: admind serves reads only, and order entry needs a producer into strategyd.'}
        </p>
      </div>
    </section>
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
        'flex h-9 items-center gap-2 rounded-lg border border-line bg-canvas/60 px-2.5',
        'transition-colors focus-within:border-accent/70',
        disabled === true && 'opacity-45',
      )}
    >
      <span className="w-14 shrink-0 text-3xs font-semibold text-fg-dim">{label}</span>
      <input
        inputMode="decimal"
        value={value}
        placeholder={placeholder}
        disabled={disabled}
        onChange={(e) => onChange(e.target.value)}
        className="num min-w-0 flex-1 bg-transparent text-right text-[13px] text-fg outline-none placeholder:text-fg-dim"
      />
      <span className="shrink-0 text-3xs text-fg-dim">{suffix}</span>
    </label>
  )
}
