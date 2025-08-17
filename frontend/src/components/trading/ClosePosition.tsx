import { useEffect, useState } from 'react'
import { Venue } from '@/components/shared/Venue'
import { cn } from '@/lib/utils'
import type { MockPosition } from '@/mock/data'

const num = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })

/** Close a position with a resting order.
 *
 *  A fixed panel rather than a popover on the row: the blotter body scrolls,
 *  and anything anchored inside it gets clipped by the scroll box at exactly
 *  the moment it matters.
 *
 *  THE SIDE IS NOT A CHOICE HERE. Closing a long is a sell and closing a short
 *  is a buy; offering the side would let a slip double the position instead of
 *  flattening it, which is the one mistake this control exists to prevent. */
export function ClosePosition({
  position,
  onClose,
}: {
  position: MockPosition
  onClose: () => void
}) {
  const side = position.qty > 0 ? 'sell' : 'buy'
  // toFixed, not a grouped format: this string is read back with Number() the
  // moment a person edits it, and `79,410.2` parses to NaN — which left the
  // submit disabled on a form that looked complete.
  const [price, setPrice] = useState(() => position.mark.toFixed(1))
  // Defaults to the whole position, because flattening is what this is for.
  // Typing over it is a partial close, which is the exception.
  const [qty, setQty] = useState(() => String(Math.abs(position.qty)))
  const [sent, setSent] = useState(false)

  useEffect(() => {
    const escape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose()
    }
    document.addEventListener('keydown', escape)
    return () => document.removeEventListener('keydown', escape)
  }, [onClose])

  const size = Number(qty)
  const over = size > Math.abs(position.qty) + 1e-12
  const armed = size > 0 && Number(price) > 0 && !over

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-canvas/70">
      <div
        className="panel w-72 shadow-[0_24px_64px_-16px_rgba(0,0,0,0.85)]"
        role="dialog"
        aria-label="Close position"
      >
        <div className="flex h-8 items-center justify-between px-3">
          <span className="panel-title">Close position</span>
          <button
            type="button"
            onClick={onClose}
            className="text-xs text-fg-dim transition-colors duration-150 hover:text-fg"
          >
            Esc
          </button>
        </div>

        <div className="flex flex-col gap-2 px-3 pb-3">
          <div className="flex items-center gap-2 rounded-sm bg-raised px-2 py-1.5">
            <Venue venue={position.venue} />
            <span className="text-sm text-fg">{position.symbol}</span>
            <span
              className={cn(
                'num ml-auto text-sm',
                position.qty > 0 ? 'text-up' : 'text-down',
              )}
            >
              {position.qty > 0 ? '+' : ''}
              {position.qty}
            </span>
          </div>

          <Field label="Price" unit="USDT" value={price} onChange={setPrice} />
          <Field
            label="Quantity"
            unit={position.symbol.split('/')[0] ?? ''}
            value={qty}
            onChange={setQty}
          />

          {/* An order larger than the position does not close it, it reverses
              it. Refused rather than warned about. */}
          {over && (
            <p className="text-2xs text-down">
              Larger than the position — this would open {num(size - Math.abs(position.qty), 4)} the
              other way.
            </p>
          )}

          <div className="flex items-baseline justify-between">
            <span className="text-2xs text-fg-dim">Order</span>
            <span className="num text-xs text-fg-mid">
              <span className={side === 'buy' ? 'text-up' : 'text-down'}>{side}</span>{' '}
              {qty === '' ? '0' : qty} at {price === '' ? '—' : price}
            </span>
          </div>

          <button
            type="button"
            disabled={!armed}
            onClick={() => setSent(true)}
            className={cn(
              'h-8 rounded-sm text-sm font-medium transition-colors duration-150',
              'disabled:cursor-not-allowed disabled:bg-raised disabled:text-fg-dim',
              armed &&
                (side === 'buy'
                  ? 'bg-up/[0.16] text-up hover:bg-up/[0.24]'
                  : 'bg-down/[0.16] text-down hover:bg-down/[0.24]'),
            )}
          >
            Create limit order
          </button>

          {sent && <p className="text-2xs text-fg-dim">Nothing is sent — reads only.</p>}
        </div>
      </div>
    </div>
  )
}

function Field({
  label,
  unit,
  value,
  onChange,
}: {
  label: string
  unit: string
  value: string
  onChange: (v: string) => void
}) {
  return (
    <label className="flex flex-col gap-1">
      <span className="text-2xs text-fg-dim">{label}</span>
      <span className="flex h-8 items-center gap-2 rounded-sm bg-raised px-2 transition-shadow duration-150 focus-within:shadow-[inset_0_0_0_1px_var(--color-accent)]">
        <input
          inputMode="decimal"
          value={value}
          onChange={(e) => onChange(e.target.value)}
          className="num min-w-0 flex-1 bg-transparent text-sm text-fg outline-none"
        />
        <span className="shrink-0 text-2xs text-fg-dim">{unit}</span>
      </span>
    </label>
  )
}
