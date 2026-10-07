import { useState, type ReactNode } from 'react'
import { api, joe, type NewParentOrder, type ParentOrder } from '@/api/client'
import { EmptyState } from '@/components/shared/EmptyState'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Silent } from '@/components/shared/Silent'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { Id, N, Outcome, Th, When } from '@/components/trading/Blotter'
import { Field } from '@/components/trading/OrderTicket'
import { cn, fixed, num, price, signed } from '@/lib/format'
import { useQuery } from '@/lib/query'

type Algo = NonNullable<NewParentOrder['algo']>
type Profile = NonNullable<NewParentOrder['profile']>
type SizeIn = 'qty' | 'notional'

const k_refresh_ms = 2_000

const k_catalogue_refresh_ms = 60_000

const k_denomination = 'USDT'

const k_algos: Algo[] = ['twap', 'vwap', 'sor', 'pov']

const k_profiles: Profile[] = ['flat', 'u', 'recorded']

const k_jitter_bp = 2000

const k_catchup_pct = 200

const k_markets: Array<'perp' | 'spot'> = ['perp', 'spot']

const k_live = new Set(['pending', 'working', 'paused'])

export function ExecutionPage() {
  const [chosen, setChosen] = useState<string | null>(null)
  const [tick, setTick] = useState(0)
  const parents = useQuery(() => joe.parents(), [tick], k_refresh_ms)
  const detail = useQuery(chosen === null ? null : () => joe.parent(chosen), [chosen, tick], k_refresh_ms)
  const refresh = () => setTick((t) => t + 1)

  // 503 is admind saying strategyd is absent or silent, not a failed request
  if (parents.state === 'failed' && parents.status === 503) {
    return (
      <div className="flex h-full min-h-0 flex-col p-2 pt-0">
        <section className="panel flex flex-1 flex-col items-center justify-center gap-2 p-6 text-center">
          <span className="text-sm text-fg">Execution is not running</span>
          <span className="max-w-[48ch] text-xs leading-relaxed text-fg-dim">
            Parent orders are worked by strategyd, which places them through gateways started with API
            keys. Run it and start admind with{' '}
            <span className="num text-fg-mid">--upstream strategyd=&lt;port&gt;</span>.
          </span>
        </section>
      </div>
    )
  }

  return (
    <div className="flex h-full min-h-0 flex-col gap-2 p-2 pt-0">
      <div className="grid min-h-0 flex-1 grid-cols-1 items-stretch gap-2 xl:grid-cols-[minmax(0,1fr)_320px]">
        <div className="flex min-h-0 flex-col gap-2">
          <section className="panel flex min-h-0 flex-1 flex-col">
            <PanelHeader
              title="Parent orders"
              right={
                parents.state === 'ready' ? (
                  <span className="num text-2xs text-fg-dim">{parents.data.items.length}</span>
                ) : undefined
              }
            />
            <div className="min-h-0 flex-1 overflow-auto">
              <Outcome q={parents} empty="">
                {(d) =>
                  d.items.length === 0 ? (
                    <EmptyState title="No parent orders yet" hint="start one from the ticket" />
                  ) : (
                    <Parents rows={d.items} chosen={chosen} onChoose={setChosen} />
                  )
                }
              </Outcome>
            </div>
          </section>

          <section className="panel flex min-h-0 flex-1 flex-col">
            {chosen === null ? (
              <>
                <PanelHeader title="Children" />
                <EmptyState title="Pick a parent order" hint="its child orders per venue show here" />
              </>
            ) : (
              <Outcome q={detail} empty="">
                {(parent) => <Detail parent={parent} onChanged={refresh} />}
              </Outcome>
            )}
          </section>
        </div>

        <Ticket
          onStarted={(id) => {
            setChosen(id)
            refresh()
          }}
        />
      </div>
    </div>
  )
}

/** positive is a cost, so it reads red */
function Bp({ value }: { value: string | undefined }) {
  const v = num(value)
  if (v === null) return <Silent />
  return <span className={v > 0 ? 'text-down' : v < 0 ? 'text-up' : 'text-fg-mid'}>{signed(v, 1)}</span>
}

function Progress({ filled, qty }: { filled: string; qty: string }) {
  const f = num(filled) ?? 0
  const q = num(qty) ?? 0
  const share = q > 0 ? Math.min(1, f / q) : 0
  return (
    <div className="flex items-center justify-end gap-2">
      <span className="num text-fg">
        {fixed(f, 4)}
        <span className="text-fg-dim"> / {fixed(q, 4)}</span>
      </span>
      <span className="relative h-[3px] w-14 shrink-0 overflow-hidden rounded-full bg-raised">
        <span className="absolute inset-y-0 left-0 bg-accent" style={{ width: `${share * 100}%` }} />
      </span>
    </div>
  )
}

const Px = ({ value }: { value: string | undefined }) =>
  value === undefined || value === '' ? <Silent /> : <>{price(value)}</>

function Parents({
  rows,
  chosen,
  onChoose,
}: {
  rows: ParentOrder[]
  chosen: string | null
  onChoose: (id: string) => void
}) {
  return (
    <table className="w-full min-w-[1080px] table-fixed border-collapse text-sm whitespace-nowrap">
      <colgroup>
        <col style={{ width: 56 }} />
        <col style={{ width: 112 }} />
        <col style={{ width: 64 }} />
        <col style={{ width: 72 }} />
        <col />
        <col style={{ width: 112 }} />
        <col style={{ width: 112 }} />
        <col style={{ width: 80 }} />
        <col style={{ width: 80 }} />
        <col style={{ width: 88 }} />
        <col style={{ width: 120 }} />
      </colgroup>
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          <Th>ID</Th>
          <Th>Symbol</Th>
          <Th>Side</Th>
          <Th>Algo</Th>
          <Th right>Filled</Th>
          <Th right>Avg</Th>
          <Th right>Arrival</Th>
          <Th right>vs arr bp</Th>
          <Th right>vs VWAP bp</Th>
          <Th>Status</Th>
          <Th right>Started</Th>
        </tr>
      </thead>
      <tbody>
        {rows.map((p) => (
          <tr
            key={p.id}
            onClick={() => onChoose(p.id)}
            className={cn('row cursor-pointer', chosen === p.id && 'bg-active')}
          >
            <Id>{p.id}</Id>
            <td className="truncate px-3 text-fg">
              {p.symbol}
              {p.market === 'spot' && <span className="text-2xs text-fg-dim"> spot</span>}
            </td>
            <td className="px-3">
              <StatusBadge status={p.side} />
            </td>
            <td className="px-3 text-fg-mid uppercase">{p.algo}</td>
            <td className="px-3">
              <Progress filled={p.filled} qty={p.qty} />
            </td>
            <N>
              <Px value={p.avg_price} />
            </N>
            <N className="text-fg-mid">
              <Px value={p.arrival} />
            </N>
            <N>
              <Bp value={p.slippage_arrival_bp} />
            </N>
            <N>
              <Bp value={p.slippage_vwap_bp} />
            </N>
            <td className="truncate px-3" title={p.reason}>
              <StatusBadge status={p.status} />
            </td>
            <td className="num px-3 text-right text-xs text-fg-mid">
              <When nanos={p.started ?? p.created} />
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

function Figure({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="text-2xs text-fg-dim">{label}</span>
      <span className="num text-sm text-fg">{children}</span>
    </div>
  )
}

function Detail({ parent, onChanged }: { parent: ParentOrder; onChanged: () => void }) {
  const [said, setSaid] = useState<string | null>(null)
  const act = async (action: 'cancel' | 'pause' | 'resume') => {
    try {
      await joe.act(parent.id, action)
      setSaid(null)
    } catch (err) {
      setSaid(err instanceof Error ? err.message : 'joe did not answer')
    }
    onChanged()
  }
  const live = k_live.has(parent.status)
  const children = parent.children ?? []
  const schedule =
    parent.algo === 'pov'
      ? `${fixed(parent.pov_bp / 100, 1)}% of volume over ${parent.horizon_s}s`
      : parent.algo === 'sor'
        ? 'at once'
        : `${parent.slices_fired}/${parent.slices} slices over ${parent.horizon_s}s`

  return (
    <>
      <PanelHeader
        title={
          <>
            <span>Parent {parent.id}</span>
            <span className="text-2xs font-normal text-fg-dim">
              {parent.symbol} · {parent.algo.toUpperCase()}
              {parent.algo === 'vwap' ? ` ${parent.profile}` : ''} · {schedule}
            </span>
          </>
        }
        right={
          live ? (
            <>
              {parent.status === 'paused' ? (
                <Action onClick={() => void act('resume')}>Resume</Action>
              ) : (
                <Action onClick={() => void act('pause')} disabled={parent.status !== 'working'}>
                  Pause
                </Action>
              )}
              <Action onClick={() => void act('cancel')} danger>
                Cancel
              </Action>
            </>
          ) : undefined
        }
      />
      <div className="flex shrink-0 flex-wrap gap-x-8 gap-y-2 px-3 pb-2">
        <Figure label="Filled">
          {parent.filled} <span className="text-fg-dim">/ {parent.qty}</span>
        </Figure>
        <Figure label="Working">{parent.working}</Figure>
        <Figure label="Avg price">
          <Px value={parent.avg_price} />
        </Figure>
        <Figure label="Arrival">
          <Px value={parent.arrival} />
        </Figure>
        <Figure label="Market VWAP">
          <Px value={parent.market_vwap} />
        </Figure>
        <Figure label="Limit">
          <Px value={parent.limit} />
        </Figure>
        <Figure label={`Fees ${k_denomination}`}>{parent.fees}</Figure>
        {parent.reason !== undefined && (
          <Figure label="Reason">
            <span className="text-down">{parent.reason}</span>
          </Figure>
        )}
      </div>
      {said !== null && <p className="px-3 pb-2 text-2xs text-down">{said}</p>}
      <div className="min-h-0 flex-1 overflow-auto">
        {children.length === 0 ? (
          <EmptyState title="No child orders yet" />
        ) : (
          <table className="w-full min-w-[880px] table-fixed border-collapse text-sm whitespace-nowrap">
            <colgroup>
              <col style={{ width: 92 }} />
              <col style={{ width: 128 }} />
              <col />
              <col style={{ width: 112 }} />
              <col style={{ width: 112 }} />
              <col style={{ width: 96 }} />
              <col style={{ width: 88 }} />
              <col style={{ width: 120 }} />
            </colgroup>
            <thead className="sticky top-0 z-10 bg-panel">
              <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
                <Th>ID</Th>
                <Th>Venue</Th>
                <Th right>Filled</Th>
                <Th right>Limit</Th>
                <Th right>Avg</Th>
                <Th right>Fee</Th>
                <Th>Status</Th>
                <Th right>Sent</Th>
              </tr>
            </thead>
            <tbody>
              {[...children].reverse().map((c) => {
                const filled = num(c.filled) ?? 0
                const qty = num(c.qty) ?? 0
                const status = c.open ? 'working' : filled >= qty ? 'filled' : filled > 0 ? 'partially_filled' : 'expired'
                return (
                  <tr key={c.client_order_id} className="row">
                    <Id>{c.client_order_id}</Id>
                    <td className="truncate px-3">
                      <Venue venue={c.venue} />
                    </td>
                    <N>
                      <span className={filled > 0 ? 'text-fg' : 'text-fg-dim'}>{c.filled}</span>
                      <span className="text-fg-dim"> / {c.qty}</span>
                    </N>
                    <N className="text-fg-mid">{price(c.limit)}</N>
                    <N>
                      <Px value={c.avg_price} />
                    </N>
                    <N className="text-fg-mid">{c.fees}</N>
                    <td className="px-3">
                      <StatusBadge status={status} />
                    </td>
                    <td className="num px-3 text-right text-xs text-fg-mid">
                      <When nanos={c.sent} />
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        )}
      </div>
    </>
  )
}

function Action({
  children,
  onClick,
  disabled,
  danger,
}: {
  children: ReactNode
  onClick: () => void
  disabled?: boolean
  danger?: boolean
}) {
  return (
    <button
      type="button"
      disabled={disabled}
      onClick={onClick}
      className={cn(
        'h-5 rounded-sm bg-raised px-2 text-2xs transition-colors duration-150 hover:bg-hover',
        'disabled:cursor-not-allowed disabled:text-fg-dim disabled:hover:bg-raised',
        danger === true ? 'text-down' : 'text-fg-mid hover:text-fg',
      )}
    >
      {children}
    </button>
  )
}

function Choice<T extends string>({
  options,
  value,
  onChange,
  label,
}: {
  options: T[]
  value: T
  onChange: (v: T) => void
  label?: (v: T) => string
}) {
  return (
    <div className="flex gap-4">
      {options.map((o) => (
        <button
          key={o}
          type="button"
          onClick={() => onChange(o)}
          className={cn(
            'relative h-6 text-xs transition-colors duration-150',
            value === o
              ? 'font-medium text-fg after:absolute after:inset-x-0 after:-bottom-px after:h-px after:bg-accent'
              : 'text-fg-dim hover:text-fg-mid',
          )}
        >
          {label === undefined ? o : label(o)}
        </button>
      ))}
    </div>
  )
}

const k_algo_label: Record<Algo, string> = { twap: 'TWAP', vwap: 'VWAP', sor: 'SOR', pov: 'POV' }

const k_profile_label: Record<Profile, string> = { flat: 'Flat', u: 'U-shape', recorded: 'Recorded' }

function Ticket({ onStarted }: { onStarted: (id: string) => void }) {
  const [symbol, setSymbol] = useState('BTC/USDT')
  const [market, setMarket] = useState<'perp' | 'spot'>('perp')
  const [sizeIn, setSizeIn] = useState<SizeIn>('notional')
  const [size, setSize] = useState('')
  const [algo, setAlgo] = useState<Algo>('twap')
  const [minutes, setMinutes] = useState('10')
  const [slices, setSlices] = useState('10')
  const [profile, setProfile] = useState<Profile>('flat')
  const [rate, setRate] = useState('10')
  const [limit, setLimit] = useState('')
  const [childCap, setChildCap] = useState('20')
  const [said, setSaid] = useState<string | null>(null)
  const [inFlight, setInFlight] = useState(false)

  const markets = useQuery(() => api.markets(), [], k_catalogue_refresh_ms)
  // joe routes by the exact local symbol, so every one a market groups is offered
  const symbols =
    markets.state === 'ready'
      ? [...new Set(markets.data.flatMap((m) => m.listings.map((l) => l.local_symbol)))]
      : []

  const base = symbol.split('/')[0] ?? ''
  const timed = algo !== 'sor'
  const sliced = algo === 'twap' || algo === 'vwap'
  const armed =
    !inFlight &&
    symbol !== '' &&
    Number(size) > 0 &&
    (!timed || Number(minutes) > 0) &&
    (!sliced || Number(slices) >= 1) &&
    (algo !== 'pov' || Number(rate) > 0)

  const submit = async (side: 'buy' | 'sell') => {
    if (!armed) return
    setInFlight(true)
    const order: NewParentOrder = {
      symbol,
      market,
      side,
      algo,
      ...(sizeIn === 'qty' ? { qty: size } : { notional: size }),
      ...(limit === '' ? {} : { limit }),
      horizon_s: timed ? Math.round(Number(minutes) * 60) : 0,
      slices: sliced ? Math.round(Number(slices)) : 1,
      jitter_bp: sliced ? k_jitter_bp : 0,
      catchup_pct: k_catchup_pct,
      profile: algo === 'vwap' ? profile : 'flat',
      pov_bp: algo === 'pov' ? Math.round(Number(rate) * 100) : 0,
      max_child_notional: childCap,
    }
    try {
      const started = await joe.start(order)
      setSaid(`started parent ${started.id}`)
      onStarted(started.id)
    } catch (err) {
      setSaid(err instanceof Error ? err.message : 'joe did not answer')
    } finally {
      setInFlight(false)
    }
  }

  return (
    <section className="panel flex flex-col overflow-visible">
      <PanelHeader title="Execution ticket" right={<span className="text-2xs text-fg-dim">joe</span>} />
      <div className="flex flex-col gap-2 px-3 pb-3">
        <label className="flex flex-col gap-1">
          <span className="text-2xs text-fg-dim">Instrument</span>
          <span className="flex h-8 items-center gap-2 rounded-sm bg-raised px-2 transition-shadow duration-150 focus-within:shadow-[inset_0_0_0_1px_var(--color-accent)]">
            <input
              list="joe-symbols"
              value={symbol}
              onChange={(e) => setSymbol(e.target.value.toUpperCase())}
              className="num min-w-0 flex-1 bg-transparent text-sm text-fg outline-none placeholder:text-fg-dim"
            />
            <datalist id="joe-symbols">
              {symbols.map((s) => (
                <option key={s} value={s} />
              ))}
            </datalist>
          </span>
        </label>
        <Choice options={k_markets} value={market} onChange={setMarket} label={(m) => (m === 'perp' ? 'Perp' : 'Spot')} />

        <Choice options={k_algos} value={algo} onChange={setAlgo} label={(a) => k_algo_label[a]} />

        <Choice
          options={['notional', 'qty'] as SizeIn[]}
          value={sizeIn}
          onChange={setSizeIn}
          label={(s) => (s === 'qty' ? 'Quantity' : 'Notional')}
        />
        <Field
          label={sizeIn === 'qty' ? 'Quantity' : 'Notional'}
          unit={sizeIn === 'qty' ? base : k_denomination}
          value={size}
          onChange={setSize}
        />

        {timed && (
          <div className="grid grid-cols-2 gap-2">
            <Field label="Horizon" unit="min" value={minutes} onChange={setMinutes} />
            {algo === 'pov' ? (
              <Field label="Participation" unit="%" value={rate} onChange={setRate} />
            ) : (
              <Field label="Slices" unit="#" value={slices} onChange={setSlices} />
            )}
          </div>
        )}
        {algo === 'vwap' && (
          <Choice options={k_profiles} value={profile} onChange={setProfile} label={(p) => k_profile_label[p]} />
        )}

        <div className="grid grid-cols-2 gap-2">
          <Field label="Limit" unit={k_denomination} value={limit} placeholder="none" onChange={setLimit} />
          <Field label="Max child" unit={k_denomination} value={childCap} onChange={setChildCap} />
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
                armed &&
                  (s === 'buy'
                    ? 'bg-up/[0.14] text-up hover:bg-up/[0.22]'
                    : 'bg-down/[0.14] text-down hover:bg-down/[0.22]'),
              )}
            >
              <span className="text-sm font-medium capitalize">
                {s} {base}
              </span>
              <span className={cn('num text-2xs', armed ? 'opacity-70' : 'text-fg-dim')}>
                {k_algo_label[algo]}
                {timed ? ` · ${minutes || 0} min` : ''}
              </span>
            </button>
          ))}
        </div>

        <p className="text-2xs leading-relaxed text-fg-dim">
          Children are IOC limits routed by price after taker fees, and each passes the risk gate.
        </p>
        {said !== null && <p className="text-2xs text-fg-dim">{said}</p>}
      </div>
    </section>
  )
}
