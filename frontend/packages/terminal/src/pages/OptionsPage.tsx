import { Fragment, useEffect, useMemo, useRef, useState } from 'react'
import {
  ChartFrame,
  cn,
  colorsFor,
  compact,
  EmptyState,
  ExchangeIcon,
  fixed,
  num,
  Pane,
  Split,
  useDismiss,
  Venue,
  venueLabel,
  Workspace,
  XYPlot,
  type XYSeries,
} from '@raikiri/ui'
import { api, type OptionSurface, type OptionUnderlying } from '../api/client'
import { crossing, instrumentsOf, liveChain, summary, type Crossing, type Leg, type Row } from '../lib/options'
import { useQuery } from '../lib/query'
import { useGreeks, useQuotes, useStreamDown } from '../lib/stream'

const k_underlyings_refresh_ms = 30_000
/** prices and venue greeks arrive over the stream; the snapshot refreshes model ivs and new strikes */
const k_chain_refresh_ms = 10_000
const k_surface_refresh_ms = 5_000

type Column =
  | 'oi'
  | 'theta'
  | 'vega'
  | 'gamma'
  | 'delta'
  | 'bidSize'
  | 'bidIv'
  | 'bid'
  | 'mark'
  | 'ask'
  | 'askIv'
  | 'askSize'

const k_columns: Array<{ key: Column; label: string }> = [
  { key: 'bidSize', label: 'Bid size' },
  { key: 'bidIv', label: 'Bid IV' },
  { key: 'bid', label: 'Bid' },
  { key: 'mark', label: 'Mark' },
  { key: 'ask', label: 'Ask' },
  { key: 'askIv', label: 'Ask IV' },
  { key: 'askSize', label: 'Ask size' },
  { key: 'delta', label: 'Delta' },
  { key: 'gamma', label: 'Gamma' },
  { key: 'vega', label: 'Vega' },
  { key: 'theta', label: 'Theta' },
  { key: 'oi', label: 'OI' },
]

/** bid to ask reads left to right on both sides; greeks sit outward */
const k_put_order: Column[] = [
  'bidSize',
  'bidIv',
  'bid',
  'mark',
  'ask',
  'askIv',
  'askSize',
  'delta',
  'gamma',
  'vega',
  'theta',
  'oi',
]
const k_call_order: Column[] = [
  'oi',
  'theta',
  'vega',
  'gamma',
  'delta',
  'bidSize',
  'bidIv',
  'bid',
  'mark',
  'ask',
  'askIv',
  'askSize',
]

const k_default_columns: Column[] = ['bidIv', 'bid', 'mark', 'ask', 'askIv', 'delta', 'oi']

/** per viewer; a failed read starts from the default */
const k_columns_key = 'raikiri.options.columns'

const k_fit_color = '#e9edf4'

function useColumns() {
  const [shown, setShown] = useState<Set<Column>>(() => {
    try {
      const saved = JSON.parse(localStorage.getItem(k_columns_key) ?? 'null') as Column[] | null
      return new Set(saved ?? k_default_columns)
    } catch {
      return new Set(k_default_columns)
    }
  })
  useEffect(() => {
    try {
      localStorage.setItem(k_columns_key, JSON.stringify([...shown]))
    } catch {
      // blocked storage keeps the choice for the session
    }
  }, [shown])
  const toggle = (c: Column) =>
    setShown((prev) => {
      const next = new Set(prev)
      if (next.has(c)) next.delete(c)
      else next.add(c)
      return next
    })
  return { shown, toggle }
}

const pct = (v: number | null, places = 1) => (v === null ? null : `${(v * 100).toFixed(places)}`)

/** plain digits, since FlashPrice reads the string back as a number */
function money(v: number, unit: Leg['unit']): string {
  // a coin premium far out of the money is a few ticks of 0.0001 or less
  if (unit === 'base')
    return v === 0 ? '0' : v.toFixed(Math.max(4, 1 - Math.floor(Math.log10(Math.abs(v)))))
  return v >= 1000 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)
}

const days = (years: string | undefined) => (years === undefined ? null : Number(years) * 365)

function expiryLabel(date: string): string {
  const d = new Date(
    Date.UTC(Number(date.slice(0, 4)), Number(date.slice(4, 6)) - 1, Number(date.slice(6, 8))),
  )
  return d.toLocaleDateString(undefined, {
    day: 'numeric',
    month: 'short',
    year: '2-digit',
    timeZone: 'UTC',
  })
}

const crossTitle = (kind: string, c: Crossing) =>
  `${kind}: sell on ${venueLabel(c.sell)}, buy on ${venueLabel(c.buy)}, +${fixed(c.pct, 2)}% before fees`

export function OptionsPage() {
  const underlyings = useQuery(() => api.optionUnderlyings(), [], k_underlyings_refresh_ms)
  const list = underlyings.state === 'ready' ? underlyings.data : []
  const [chosen, setChosen] = useState<string | null>(null)
  const underlying =
    list.find((u) => u.underlying === chosen) ??
    list.find((u) => u.underlying === 'BTC') ??
    list[0] ??
    null
  const [pickedExpiry, setPickedExpiry] = useState<string | null>(null)
  const expiries = underlying?.expiries ?? []
  const expiry = expiries.find((e) => e.expiry === pickedExpiry) ?? expiries[0] ?? null
  const [venues, setVenues] = useState<Set<string>>(new Set())
  const venueParam = venues.size === 0 ? undefined : [...venues].sort().join(',')
  const [native, setNative] = useState(false)
  const [expandAll, setExpandAll] = useState(false)
  const columns = useColumns()

  const name = underlying?.underlying ?? null
  const chain = useQuery(
    name !== null && expiry !== null
      ? () => api.optionChain(name, expiry.expiry, venueParam)
      : null,
    [name, expiry?.expiry, venueParam],
    k_chain_refresh_ms,
  )
  const surface = useQuery(
    name !== null ? () => api.optionSurface(name, venueParam) : null,
    [name, venueParam],
    k_surface_refresh_ms,
  )
  // an answer for the previous expiry is not drawn under the new tab
  const snapshot =
    chain.state === 'ready' &&
    chain.data.expiry === expiry?.expiry &&
    chain.data.underlying === name
      ? chain.data
      : null
  const ids = useMemo(() => (snapshot === null ? [] : instrumentsOf(snapshot)), [snapshot])
  const quotes = useQuotes(ids)
  const greeks = useGreeks(ids)
  const streamDown = useStreamDown()
  const live = useMemo(
    () => (snapshot === null ? null : liveChain(snapshot, quotes, greeks)),
    [snapshot, quotes, greeks],
  )
  const fits = surface.state === 'ready' && surface.data.underlying === name ? surface.data : null

  const allVenues = underlying?.venues.map((v) => v.venue) ?? []
  const colors = useMemo(() => colorsFor(allVenues), [allVenues.join()])

  return (
    <Workspace
      page="options"
      panels={{
        chain: (
          <section className="panel flex flex-col">
            <ChainHeader
              list={list}
              underlying={underlying}
              onUnderlying={(u) => {
                setChosen(u)
                setPickedExpiry(null)
                setVenues(new Set())
              }}
              expiry={expiry?.expiry ?? null}
              onExpiry={setPickedExpiry}
              venues={venues}
              onVenues={setVenues}
              native={native}
              onNative={setNative}
              expandAll={expandAll}
              onExpandAll={setExpandAll}
              columns={columns}
              streamDown={streamDown}
            />
            {snapshot !== null && (
              <div className="flex h-7 shrink-0 items-center gap-5 px-3 text-xs text-fg-dim">
                <span>
                  Forward{' '}
                  <span className="num text-fg">
                    {snapshot.forward ? fixed(Number(snapshot.forward), 1) : '—'}
                  </span>{' '}
                  <span className="text-2xs">{snapshot.forward_source}</span>
                </span>
                <span>
                  ATM{' '}
                  <span className="num text-fg">
                    {snapshot.atm_strike ? compact(Number(snapshot.atm_strike)) : '—'}
                  </span>
                </span>
                <span>
                  Expires in{' '}
                  <span className="num text-fg">
                    {fixed(snapshot.seconds_to_expiry / 86_400, 2)}d
                  </span>
                </span>
                <span className="ml-auto flex items-center gap-3">
                  {snapshot.venues.map((v) => (
                    <span key={v.venue} title="the rate a coin premium turns into usd at">
                      {venueLabel(v.venue)} index{' '}
                      <span className="num text-fg-mid">
                        {v.index ? fixed(Number(v.index), 1) : '—'}
                      </span>
                    </span>
                  ))}
                </span>
              </div>
            )}
            <div className="min-h-0 flex-1 overflow-auto">
              {live !== null ? (
                <ChainTable
                  rows={live.rows}
                  forward={live.forward}
                  atm={live.atm}
                  shown={columns.shown}
                  native={native}
                  expandAll={expandAll}
                  venueCount={snapshot?.venues.length ?? 0}
                />
              ) : (
                <EmptyState
                  title={
                    underlyings.state === 'failed'
                      ? 'Options unavailable'
                      : underlyings.state === 'ready' && list.length === 0
                        ? 'No options listed'
                        : chain.state === 'failed'
                          ? 'Chain unavailable'
                          : 'Asking admind'
                  }
                  hint={
                    underlyings.state === 'failed'
                      ? underlyings.title
                      : chain.state === 'failed'
                        ? chain.title
                        : underlyings.state === 'ready' && list.length === 0
                          ? 'No attached gateway lists options.'
                          : undefined
                  }
                />
              )}
            </div>
          </section>
        ),
        smile: <SmileChart surface={fits} expiry={expiry?.expiry ?? null} colors={colors} />,
        term: <TermChart surface={fits} expiry={expiry?.expiry ?? null} />,
        skew: <SkewChart surface={fits} />,
      }}
    >
      <Split id="columns">
        <Pane slot="chain" size={66} min={35} />
        <Pane size={34} min={18}>
          <Split id="charts" vertical>
            <Pane slot="smile" size={40} min={20} />
            <Pane slot="term" size={30} min={15} />
            <Pane slot="skew" size={30} min={15} />
          </Split>
        </Pane>
      </Split>
    </Workspace>
  )
}

function Tab({
  active,
  onClick,
  children,
}: {
  active: boolean
  onClick: () => void
  children: React.ReactNode
}) {
  return (
    <button
      type="button"
      role="tab"
      aria-selected={active}
      onClick={onClick}
      className={cn(
        'relative flex h-8 shrink-0 items-center gap-1.5 text-xs whitespace-nowrap transition-colors duration-150',
        active
          ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
          : 'text-fg-dim hover:text-fg-mid',
      )}
    >
      {children}
    </button>
  )
}

function Toggle({
  on,
  onClick,
  children,
  title,
}: {
  on: boolean
  onClick: () => void
  children: React.ReactNode
  title?: string
}) {
  return (
    <button
      type="button"
      title={title}
      aria-pressed={on}
      onClick={onClick}
      className={cn(
        'flex h-6 items-center gap-1.5 rounded-sm px-2 text-xs transition-colors duration-150',
        on ? 'bg-active text-fg' : 'text-fg-dim hover:bg-hover hover:text-fg-mid',
      )}
    >
      {children}
    </button>
  )
}

function ChainHeader({
  list,
  underlying,
  onUnderlying,
  expiry,
  onExpiry,
  venues,
  onVenues,
  native,
  onNative,
  expandAll,
  onExpandAll,
  columns,
  streamDown,
}: {
  list: OptionUnderlying[]
  underlying: OptionUnderlying | null
  onUnderlying: (u: string) => void
  expiry: string | null
  onExpiry: (e: string) => void
  venues: Set<string>
  onVenues: (v: Set<string>) => void
  native: boolean
  onNative: (n: boolean) => void
  expandAll: boolean
  onExpandAll: (e: boolean) => void
  columns: { shown: Set<Column>; toggle: (c: Column) => void }
  streamDown: boolean
}) {
  const [chooser, setChooser] = useState(false)
  const chooserRef = useRef<HTMLDivElement>(null)
  useDismiss(chooserRef, chooser, setChooser)
  return (
    <div data-drag-handle className="flex shrink-0 flex-col">
      <div className="flex h-8 items-center gap-4 px-3" role="tablist" aria-label="Underlying">
        {list.map((u) => (
          <Tab
            key={u.underlying}
            active={u.underlying === underlying?.underlying}
            onClick={() => onUnderlying(u.underlying)}
          >
            {u.underlying}
            <span className="num text-2xs text-fg-dim">{u.venues.length}</span>
          </Tab>
        ))}
        <div className="ml-auto flex items-center gap-1.5">
          {(underlying?.venues ?? []).map((v) => (
            <Toggle
              key={v.venue}
              on={venues.has(v.venue)}
              title={`${v.options} options`}
              onClick={() => {
                const next = new Set(venues)
                if (next.has(v.venue)) next.delete(v.venue)
                else next.add(v.venue)
                onVenues(next)
              }}
            >
              <ExchangeIcon venue={v.venue} className="size-[13px]" />
              {venueLabel(v.venue)}
            </Toggle>
          ))}
          <span className="mx-1 h-4 w-px bg-line" />
          <Toggle
            on={!native}
            onClick={() => onNative(!native)}
            title="venue rows in usd or in each venue's premium unit"
          >
            {native ? 'Native' : 'USD'}
          </Toggle>
          <Toggle
            on={expandAll}
            onClick={() => onExpandAll(!expandAll)}
            title="every venue under each strike"
          >
            Venues
          </Toggle>
          <div ref={chooserRef} className="relative">
            <Toggle on={chooser} onClick={() => setChooser(!chooser)}>
              Columns
            </Toggle>
            {chooser && (
              <div className="absolute top-7 right-0 z-30 w-36 rounded-md bg-raised p-1.5 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.7)]">
                {k_columns.map((c) => (
                  <label
                    key={c.key}
                    className="flex h-6 cursor-pointer items-center gap-2 rounded-sm px-1.5 text-xs text-fg-mid hover:bg-hover"
                  >
                    <input
                      type="checkbox"
                      checked={columns.shown.has(c.key)}
                      onChange={() => columns.toggle(c.key)}
                    />
                    {c.label}
                  </label>
                ))}
              </div>
            )}
          </div>
          <span
            className={cn('ml-1 size-[5px] rounded-full', streamDown ? 'bg-down' : 'bg-up')}
            title={streamDown ? 'stream unavailable' : 'live'}
          />
        </div>
      </div>
      <div
        className="flex h-8 items-center gap-4 overflow-x-auto px-3"
        role="tablist"
        aria-label="Expiry"
      >
        {(underlying?.expiries ?? []).map((e) => (
          <Tab key={e.expiry} active={e.expiry === expiry} onClick={() => onExpiry(e.expiry)}>
            {expiryLabel(e.date)}
            <span className="num text-2xs text-fg-dim">{e.venues.length}</span>
          </Tab>
        ))}
      </div>
    </div>
  )
}

function Cell({
  leg,
  column,
  native,
  best,
  shade = false,
}: {
  leg: Leg
  column: Column
  native: boolean
  best?: { bid: boolean; ask: boolean } | undefined
  /** in the money */
  shade?: boolean
}) {
  const unit = native ? leg.unit : 'quote'
  const priced = (v: number | null, usdV: number | null) => (native ? v : usdV)
  const tip = (v: number | null, usdV: number | null) =>
    leg.unit === 'base' && v !== null && usdV !== null && leg.rate !== null
      ? `${v.toFixed(4)} coin = ${fixed(usdV, 2)} usd at ${fixed(leg.rate, 1)}`
      : undefined
  const model = (flag: boolean) => (flag ? 'text-fg-mid italic' : '')
  const dash = <span className="text-fg-dim">—</span>
  switch (column) {
    case 'bid': {
      const v = priced(leg.bid, leg.bidUsd)
      return (
        <td
          className={cn('px-2 text-right', shade && 'bg-raised/25', best?.bid && 'bg-up/10')}
          title={tip(leg.bid, leg.bidUsd)}
        >
          {v === null ? dash : <span className={cn('num', 'text-up')}>{money(v, unit)}</span>}
        </td>
      )
    }
    case 'ask': {
      const v = priced(leg.ask, leg.askUsd)
      return (
        <td
          className={cn('px-2 text-right', shade && 'bg-raised/25', best?.ask && 'bg-down/10')}
          title={tip(leg.ask, leg.askUsd)}
        >
          {v === null ? dash : <span className={cn('num', 'text-down')}>{money(v, unit)}</span>}
        </td>
      )
    }
    case 'mark': {
      const v = priced(leg.mark, leg.markUsd)
      return (
        <td
          className={cn('num px-2 text-right text-fg', shade && 'bg-raised/25')}
          title={tip(leg.mark, leg.markUsd)}
        >
          {v === null ? dash : money(v, unit)}
        </td>
      )
    }
    case 'bidIv':
    case 'askIv': {
      const v = column === 'bidIv' ? leg.bidIv : leg.askIv
      return (
        <td
          className={cn(
            'num px-2 text-right text-fg-mid',
            shade && 'bg-raised/25',
            model(leg.ivModel),
          )}
          title={leg.ivModel ? 'implied here' : undefined}
        >
          {pct(v) ?? dash}
        </td>
      )
    }
    case 'bidSize':
    case 'askSize': {
      const v = column === 'bidSize' ? leg.bidSize : leg.askSize
      return (
        <td className={cn('num px-2 text-right text-fg-dim', shade && 'bg-raised/25')}>
          {v === null ? dash : compact(v)}
        </td>
      )
    }
    case 'delta':
    case 'gamma':
    case 'vega':
    case 'theta': {
      const v = leg[column]
      const text =
        v === null
          ? null
          : column === 'delta'
            ? v.toFixed(3)
            : column === 'gamma'
              ? v.toPrecision(3)
              : fixed(v, 2)
      return (
        <td
          className={cn(
            'num px-2 text-right text-fg-mid',
            shade && 'bg-raised/25',
            model(leg.greeksModel),
          )}
          title={leg.greeksModel ? 'black-76 on the forward' : undefined}
        >
          {text ?? dash}
        </td>
      )
    }
    case 'oi':
      return (
        <td className={cn('num px-2 text-right text-fg-dim', shade && 'bg-raised/25')}>
          {leg.oi === null ? dash : compact(leg.oi)}
        </td>
      )
  }
}

const k_empty_leg: Leg = {
  venue: '',
  id: '',
  symbol: '',
  unit: 'quote',
  rate: null,
  bid: null,
  ask: null,
  bidSize: null,
  askSize: null,
  mark: null,
  bidUsd: null,
  askUsd: null,
  markUsd: null,
  bidIv: null,
  askIv: null,
  markIv: null,
  ivModel: false,
  delta: null,
  gamma: null,
  vega: null,
  theta: null,
  greeksModel: false,
  oi: null,
}

function BestVenue({ venue }: { venue: string | undefined }) {
  if (venue === undefined) return null
  return (
    <ExchangeIcon venue={venue} className="ml-1 inline-block size-[11px] align-[-1px] opacity-80" />
  )
}

function ChainTable({
  rows,
  forward,
  atm,
  shown,
  native,
  expandAll,
  venueCount,
}: {
  rows: Row[]
  forward: number | null
  atm: number | null
  shown: Set<Column>
  native: boolean
  expandAll: boolean
  venueCount: number
}) {
  const [open, setOpen] = useState<Set<number>>(new Set())
  const calls = k_call_order.filter((c) => shown.has(c))
  const puts = k_put_order.filter((c) => shown.has(c))
  const label = (c: Column) => k_columns.find((x) => x.key === c)?.label ?? c
  const forwardAfter = forward === null ? -1 : rows.findIndex((r) => r.strike > forward)
  const span = calls.length + puts.length + 1

  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 text-2xs">
          <th colSpan={calls.length} className="col-head px-2 text-center">
            Calls
          </th>
          <th className="col-head px-2" />
          <th colSpan={puts.length} className="col-head px-2 text-center">
            Puts
          </th>
        </tr>
        <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          {calls.map((c) => (
            <th key={`c${c}`} className="col-head px-2 text-right">
              {label(c)}
            </th>
          ))}
          <th className="col-head bg-raised/60 px-3 text-center">Strike</th>
          {puts.map((c) => (
            <th key={`p${c}`} className="col-head px-2 text-right">
              {label(c)}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((row, i) => {
          const call = summary(row.call) ?? k_empty_leg
          const put = summary(row.put) ?? k_empty_leg
          const expanded = expandAll || open.has(row.strike)
          const venueNames = [
            ...new Set([...row.call.legs, ...row.put.legs].map((l) => l.venue)),
          ].sort()
          const itmCall = forward !== null && row.strike < forward
          const itmPut = forward !== null && row.strike > forward
          const callCross = crossing(row.call)
          const putCross = crossing(row.put)
          return (
            <Fragment key={row.strike}>
              {i === forwardAfter && forward !== null && (
                <tr className="h-5" data-forward>
                  <td colSpan={span} className="relative px-3">
                    <div className="absolute inset-x-0 top-1/2 h-px bg-accent/50" />
                    <span className="relative mx-auto block w-fit rounded-sm bg-panel px-2 text-2xs text-accent">
                      Forward {fixed(forward, 1)}
                    </span>
                  </td>
                </tr>
              )}
              <tr
                className="row h-7 cursor-pointer"
                onClick={() =>
                  setOpen((prev) => {
                    const next = new Set(prev)
                    if (next.has(row.strike)) next.delete(row.strike)
                    else next.add(row.strike)
                    return next
                  })
                }
              >
                {calls.map((c) => (
                  <SummaryCell
                    key={`c${c}`}
                    leg={call}
                    column={c}
                    itm={itmCall}
                    best={
                      c === 'bid'
                        ? row.call.bestBid?.venue
                        : c === 'ask'
                          ? row.call.bestAsk?.venue
                          : undefined
                    }
                    multi={venueCount > 1}
                  />
                ))}
                <td
                  className={cn(
                    'num bg-raised/60 px-3 text-center font-medium',
                    row.strike === atm ? 'text-accent' : 'text-fg',
                  )}
                  title={[
                    row.strike === atm ? 'at the money' : null,
                    callCross === null ? null : crossTitle('call', callCross),
                    putCross === null ? null : crossTitle('put', putCross),
                  ]
                    .filter((t) => t !== null)
                    .join('\n')}
                >
                  {callCross !== null && <span className="mr-1.5 text-2xs text-up">◂ +{fixed(callCross.pct, 1)}%</span>}
                  {row.strike.toLocaleString('en-US')}
                  {putCross !== null && <span className="ml-1.5 text-2xs text-up">+{fixed(putCross.pct, 1)}% ▸</span>}
                </td>
                {puts.map((c) => (
                  <SummaryCell
                    key={`p${c}`}
                    leg={put}
                    column={c}
                    itm={itmPut}
                    best={
                      c === 'bid'
                        ? row.put.bestBid?.venue
                        : c === 'ask'
                          ? row.put.bestAsk?.venue
                          : undefined
                    }
                    multi={venueCount > 1}
                  />
                ))}
              </tr>
              {expanded &&
                venueNames.map((venue) => {
                  const cl = row.call.legs.find((l) => l.venue === venue) ?? k_empty_leg
                  const pl = row.put.legs.find((l) => l.venue === venue) ?? k_empty_leg
                  return (
                    <tr key={venue} className="h-6 bg-canvas/40 text-xs" data-venue={venue}>
                      {calls.map((c) => (
                        <Cell
                          key={`c${c}`}
                          leg={cl}
                          column={c}
                          native={native}
                          best={{
                            bid: row.call.bestBid?.venue === venue,
                            ask: row.call.bestAsk?.venue === venue,
                          }}
                        />
                      ))}
                      <td className="bg-raised/30 px-3 text-center">
                        <Venue venue={venue} className="text-2xs" />
                      </td>
                      {puts.map((c) => (
                        <Cell
                          key={`p${c}`}
                          leg={pl}
                          column={c}
                          native={native}
                          best={{
                            bid: row.put.bestBid?.venue === venue,
                            ask: row.put.bestAsk?.venue === venue,
                          }}
                        />
                      ))}
                    </tr>
                  )
                })}
            </Fragment>
          )
        })}
      </tbody>
    </table>
  )
}

function SummaryCell({
  leg,
  column,
  itm,
  best,
  multi,
}: {
  leg: Leg
  column: Column
  itm: boolean
  best: string | undefined
  multi: boolean
}) {
  // the summary is the best across venues, so its quote carries the venue that set it
  if ((column === 'bid' || column === 'ask') && multi && best !== undefined) {
    const v = column === 'bid' ? leg.bidUsd : leg.askUsd
    return (
      <td
        className={cn('px-2 text-right', itm && 'bg-raised/25')}
        title={`best ${column} on ${venueLabel(best)}`}
      >
        {v === null ? (
          <span className="text-fg-dim">—</span>
        ) : (
          <>
            <span className={cn('num', column === 'bid' ? 'text-up' : 'text-down')}>
              {money(v, 'quote')}
            </span>
            <BestVenue venue={best} />
          </>
        )}
      </td>
    )
  }
  return <Cell leg={leg} column={column} native={false} shade={itm} />
}

function useSlice(surface: OptionSurface | null, expiry: string | null) {
  return surface?.expiries.find((e) => e.expiry === expiry) ?? null
}

function SmileChart({
  surface,
  expiry,
  colors,
}: {
  surface: OptionSurface | null
  expiry: string | null
  colors: Map<string, string>
}) {
  const slice = useSlice(surface, expiry)
  const [highlight, setHighlight] = useState<string | null>(null)
  const series = useMemo<XYSeries[]>(() => {
    if (slice === null) return []
    const byVenue = new Map<string, Array<{ x: number; y: number }>>()
    for (const p of slice.smile) {
      byVenue.set(p.venue, [
        ...(byVenue.get(p.venue) ?? []),
        { x: Number(p.strike), y: Number(p.mid_iv) * 100 },
      ])
    }
    const out: XYSeries[] = [...byVenue].map(([venue, points]) => ({
      key: venue,
      label: venueLabel(venue),
      color: colors.get(venue) ?? '#94a3b8',
      points,
      mode: 'dots',
    }))
    if (slice.fitted.length > 1) {
      out.push({
        key: 'fit',
        label: surface?.model ?? 'fit',
        color: k_fit_color,
        points: slice.fitted.map((p) => ({ x: Number(p.strike), y: Number(p.vol) * 100 })),
        mode: 'line',
      })
    }
    return out
  }, [slice, colors, surface?.model])
  const forward = num(slice?.forward)
  return (
    <ChartFrame
      title="Smile"
      note={
        slice === null
          ? undefined
          : `${expiryLabel(slice.date)} · mid iv, out of the money · ${surface?.model ?? ''} fit`
      }
      legend={series.map((s) => ({ key: s.key, label: s.label, color: s.color }))}
      onLegendHover={setHighlight}
      readout={
        slice === null ? undefined : (
          <div className="flex gap-3 text-2xs text-fg-dim">
            <span>
              ATM <span className="num text-fg">{pct(num(slice.atm_iv)) ?? '—'}</span>
            </span>
            <span>
              RR <span className="num text-fg">{pct(num(slice.rr25), 2) ?? '—'}</span>
            </span>
            <span>
              BF <span className="num text-fg">{pct(num(slice.bf25), 2) ?? '—'}</span>
            </span>
          </div>
        )
      }
      height={260}
      placeholder={
        series.length === 0
          ? surface === null
            ? 'No surface yet'
            : 'Nothing quoted on this expiry'
          : undefined
      }
    >
      {({ width, height }) => (
        <XYPlot
          series={series}
          width={width}
          height={height}
          formatX={(x) => compact(x)}
          formatY={(y) => `${y.toFixed(0)}%`}
          markers={forward === null ? [] : [{ x: forward, label: 'F' }]}
          highlight={highlight}
        />
      )}
    </ChartFrame>
  )
}

function TermChart({ surface, expiry }: { surface: OptionSurface | null; expiry: string | null }) {
  const points = (surface?.expiries ?? [])
    .map((e) => ({ x: days(e.years), y: num(e.atm_iv) }))
    .filter((p): p is { x: number; y: number } => p.x !== null && p.y !== null)
    .map((p) => ({ x: p.x, y: p.y * 100 }))
  const chosen = days(useSlice(surface, expiry)?.years)
  return (
    <ChartFrame
      title="ATM term structure"
      note="atm iv by days to expiry"
      height={200}
      placeholder={points.length === 0 ? 'No surface yet' : undefined}
    >
      {({ width, height }) => (
        <XYPlot
          series={[{ key: 'atm', label: 'ATM iv', color: '#4c9aff', points, mode: 'both' }]}
          width={width}
          height={height}
          formatX={(x) => `${x.toFixed(0)}d`}
          formatY={(y) => `${y.toFixed(1)}%`}
          markers={chosen === null ? [] : [{ x: chosen, label: 'shown' }]}
        />
      )}
    </ChartFrame>
  )
}

function SkewChart({ surface }: { surface: OptionSurface | null }) {
  const series = (key: 'rr25' | 'bf25', label: string, color: string): XYSeries => ({
    key,
    label,
    color,
    mode: 'both',
    points: (surface?.expiries ?? [])
      .map((e) => ({ x: days(e.years), y: num(e[key]) }))
      .filter((p): p is { x: number; y: number } => p.x !== null && p.y !== null)
      .map((p) => ({ x: p.x, y: p.y * 100 })),
  })
  const both = [
    series('rr25', '25Δ risk reversal', '#a78bfa'),
    series('bf25', '25Δ butterfly', '#f59e0b'),
  ]
  return (
    <ChartFrame
      title="25Δ RR and BF"
      note="vol points by days to expiry"
      legend={both.map((s) => ({ key: s.key, label: s.label, color: s.color }))}
      height={200}
      placeholder={both.every((s) => s.points.length === 0) ? 'No fit yet' : undefined}
    >
      {({ width, height }) => (
        <XYPlot
          series={both}
          width={width}
          height={height}
          formatX={(x) => `${x.toFixed(0)}d`}
          formatY={(y) => y.toFixed(1)}
        />
      )}
    </ChartFrame>
  )
}
