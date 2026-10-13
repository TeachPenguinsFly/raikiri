import { useRef, useState } from 'react'
import { cn, ExchangeIcon, fixed, Icon, price, spreadBp, useDismiss } from '@raikiri/ui'
import { api, type AccountBalances, placeOrder } from '../../api/client'
import type { LiveListing } from '../../lib/live'
import { useQuery } from '../../lib/query'

type Kind = 'limit' | 'market'

const k_balances_refresh_ms = 10_000

const k_denomination = 'USDT'

const k_size_fractions = [0.25, 0.5, 1]

/** a long price steps down a size so the whole figure still fits the touch */
const touchSize = (decimal: string | null) =>
  decimal !== null && price(decimal).length > 10 ? 'text-xs' : 'text-sm'

/** a null free balance refuses sizing rather than using the committed total */
function freeBalance(accounts: AccountBalances[], account: string): number | null {
  const v = accounts.find((a) => a.account === account)?.assets.find((x) => x.asset === k_denomination)?.available
  return v === undefined || v === null ? null : Number(v)
}

export function OrderTicket({
  listings,
  venue,
  onVenue,
}: {
  listings: LiveListing[]
  venue: string | null
  onVenue: (venue: string) => void
}) {
  const [kind, setKind] = useState<Kind>('limit')
  const [limit, setLimit] = useState('')
  const [qty, setQty] = useState('')
  const [sent, setSent] = useState<string | null>(null)
  const [inFlight, setInFlight] = useState(false)

  const [seededFor, setSeededFor] = useState<string | null>(null)

  const balances = useQuery(() => api.balances(), [], k_balances_refresh_ms)

  const listing = listings.find((l) => l.venue === venue) ?? null

  // the limit starts at the first bid of each instrument; after that only a touch press moves it
  if (listing !== null && listing.bid !== null && seededFor !== listing.instrument_id) {
    setSeededFor(listing.instrument_id)
    setLimit(price(listing.bid))
  }

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

  const base = listing.exchange_symbol.replace(/USDT.*|_.*|-.*/, '')
  // a market order works at the ask; null until the venue quotes
  const working = kind === 'market' ? listing.askValue : Number(limit)
  const notional = working === null ? null : working * Number(qty)
  const armed =
    !inFlight && Number(qty) > 0 && (kind === 'market' ? listing.ask !== null : Number(limit) > 0)
  const free = balances.state === 'ready' ? freeBalance(balances.data, listing.account) : null
  const spread = spreadBp(listing.bidValue, listing.askValue)

  const submit = async (side: 'buy' | 'sell') => {
    if (!armed) return
    setInFlight(true)
    try {
      const accepted = await placeOrder(
        {
          account: listing.account,
          instrument_id: listing.instrument_id,
          side,
          order_type: kind,
          tif: kind === 'market' ? 'ioc' : 'gtc',
          ...(kind === 'limit' ? { price: limit } : {}),
          qty,
          post_only: false,
          reduce_only: false,
        },
        crypto.randomUUID(),
      )
      setSent(`queued ${accepted.client_order_id}`)
    } catch (err) {
      setSent(err instanceof Error ? err.message : 'the desk did not answer')
    } finally {
      setInFlight(false)
    }
  }

  return (
    <section className="panel flex flex-col overflow-visible">
      <div className="flex h-7 items-center justify-between px-3">
        <span className="panel-title">Ticket</span>
        <span className="text-2xs text-fg-dim">{listing.exchange_symbol}</span>
      </div>

      <div className="flex flex-col gap-2 px-3 pb-3">
        <VenuePicker listings={listings} chosen={listing} onChoose={onVenue} />

        <div className="flex items-stretch gap-1">
          <button
            type="button"
            disabled={listing.bid === null}
            onClick={() => listing.bid !== null && setLimit(price(listing.bid))}
            className={cn(
              'num flex h-8 min-w-0 flex-1 items-center justify-center rounded-sm bg-up/[0.10] text-up transition-colors duration-150 hover:bg-up/[0.16]',
              touchSize(listing.bid),
            )}
          >
            {listing.bid === null ? '—' : price(listing.bid)}
          </button>
          <span className="num flex w-14 shrink-0 items-center justify-center text-2xs text-fg-dim">
            {spread === null ? '—' : `${spread.toFixed(1)} bp`}
          </span>
          <button
            type="button"
            disabled={listing.ask === null}
            onClick={() => listing.ask !== null && setLimit(price(listing.ask))}
            className={cn(
              'num flex h-8 min-w-0 flex-1 items-center justify-center rounded-sm bg-down/[0.10] text-down transition-colors duration-150 hover:bg-down/[0.16]',
              touchSize(listing.ask),
            )}
          >
            {listing.ask === null ? '—' : price(listing.ask)}
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
          unit={k_denomination}
          value={kind === 'market' ? '' : limit}
          placeholder={kind === 'market' && listing.ask !== null ? price(listing.ask) : ''}
          disabled={kind === 'market'}
          onChange={setLimit}
        />
        <Field label="Quantity" unit={base} value={qty} onChange={setQty} />

        <div className="flex gap-1" title={free === null ? 'this venue publishes no free balance' : undefined}>
          {k_size_fractions.map((f) => (
            <button
              key={f}
              type="button"
              disabled={free === null || working === null || working <= 0}
              onClick={() =>
                free !== null &&
                working !== null &&
                working > 0 &&
                setQty(((free * f) / working).toFixed(4))
              }
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

        <div className="grid grid-cols-2 gap-1.5 pt-0.5">
          {(['buy', 'sell'] as const).map((s) => (
            <button
              key={s}
              type="button"
              disabled={!armed}
              onClick={() => void submit(s)}
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
                {notional !== null && Number.isFinite(notional) && notional > 0
                  ? `${fixed(notional)} ${k_denomination}`
                  : `0 ${base}`}
              </span>
            </button>
          ))}
        </div>

        {sent !== null && <p className="text-2xs text-fg-dim">{sent}</p>}
      </div>
    </section>
  )
}

function VenuePicker({
  listings,
  chosen,
  onChoose,
}: {
  listings: LiveListing[]
  chosen: LiveListing
  onChoose: (venue: string) => void
}) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  useDismiss(box, open, setOpen)

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
        <span className="num ml-auto text-xs text-fg-dim">
          {chosen.bid === null ? '—' : price(chosen.bid)}
        </span>
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
              <span className="num ml-auto text-xs text-up">
                {l.bid === null ? '—' : price(l.bid)}
              </span>
              <span className="num min-w-[52px] text-right text-xs text-down">
                {l.ask === null ? '—' : price(l.ask)}
              </span>
            </button>
          ))}
        </div>
      )}
    </div>
  )
}

export function Field({
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
