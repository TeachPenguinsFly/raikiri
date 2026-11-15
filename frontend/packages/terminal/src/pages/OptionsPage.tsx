import { Fragment, useEffect, useMemo, useState } from 'react'
import { useSearchParams } from 'react-router'
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
  Venue,
  venueLabel,
  Workspace,
  XYPlot,
  type XYSeries,
} from '@raikiri/ui'
import { CoinIcon, Info, Picker, Seg } from '@raikiri/ui/v3'
import { api, type OptionChain, type OptionSurface, type OptionUnderlying } from '../api/client'
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

/** every column keeps its width whatever the digits in it, so a moving price never shifts the grid */
const k_width: Record<Column, number> = {
  oi: 64,
  theta: 72,
  vega: 72,
  gamma: 88,
  delta: 76,
  bidSize: 76,
  bidIv: 72,
  bid: 104,
  mark: 84,
  ask: 104,
  askIv: 72,
  askSize: 76,
}
const k_strike_width = 168

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
  if (unit === 'base') return v === 0 ? '0' : v.toFixed(Math.max(4, 1 - Math.floor(Math.log10(Math.abs(v)))))
  return v >= 1000 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)
}

const days = (years: string | undefined) => (years === undefined ? null : Number(years) * 365)

const k_months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']

const parts = (date: string) =>
  [Number(date.slice(0, 4)), Number(date.slice(4, 6)) - 1, Number(date.slice(6, 8))] as const

/** 16 Nov, with the year only when it is not this one */
function expiryLabel(date: string): string {
  const [y, m, d] = parts(date)
  const label = `${d} ${k_months[m]}`
  return y === new Date().getUTCFullYear() ? label : `${label} '${String(y).slice(2)}`
}

const monthOf = (date: string) => {
  const [y, m] = parts(date)
  return `${k_months[m]} ${y}`
}

/** days to an expiry, which venues settle at 08:00 utc */
function daysTo(date: string): number {
  const [y, m, d] = parts(date)
  return (Date.UTC(y, m, d, 8) - Date.now()) / 86_400_000
}

function lasting(days: number): string {
  if (days < 2) {
    const h = Math.max(0, Math.floor(days * 24))
    const min = Math.max(0, Math.floor((days * 24 - h) * 60))
    return h === 0 ? `${min} min` : `${h} h ${min} min`
  }
  return `${Math.round(days)} days`
}

const shortLasting = (days: number) => (days < 1 ? `${Math.max(0, Math.round(days * 24))}h` : `${Math.round(days)}d`)

const crossTitle = (kind: string, c: Crossing) =>
  `${kind}: sell on ${venueLabel(c.sell)}, buy on ${venueLabel(c.buy)}, +${fixed(c.pct, 2)}% before fees`

export function OptionsPage() {
  const underlyings = useQuery(() => api.optionUnderlyings(), [], k_underlyings_refresh_ms)
  const list = underlyings.state === 'ready' ? underlyings.data : []
  // ?u= opens an underlying, e.g. from the terminal's market list
  const [params, setParams] = useSearchParams()
  const chosen = params.get('u')
  const setChosen = (u: string) =>
    setParams(
      (prev) => {
        const next = new URLSearchParams(prev)
        next.set('u', u)
        return next
      },
      { replace: true },
    )
  const underlying =
    list.find((u) => u.underlying === chosen) ?? list.find((u) => u.underlying === 'BTC') ?? list[0] ?? null
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
    name !== null && expiry !== null ? () => api.optionChain(name, expiry.expiry, venueParam) : null,
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
    chain.state === 'ready' && chain.data.expiry === expiry?.expiry && chain.data.underlying === name
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
            />
            <ChainFacts snapshot={snapshot}>
              <ChainControls
                underlying={underlying}
                venues={venues}
                onVenues={setVenues}
                native={native}
                onNative={setNative}
                expandAll={expandAll}
                onExpandAll={setExpandAll}
                columns={columns}
                streamDown={streamDown}
              />
            </ChainFacts>
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

function Fact({ label, children, info }: { label: string; children: React.ReactNode; info?: React.ReactNode }) {
  return (
    <span className="flex items-center gap-2 whitespace-nowrap">
      <span className="text-fg-dim">{label}</span>
      <span className="mono text-fg">{children}</span>
      {info !== undefined && <Info>{info}</Info>}
    </span>
  )
}

/** the expiry's reference numbers: where the money is, how long is left, what each venue converts premiums at */
function ChainFacts({ snapshot, children }: { snapshot: OptionChain | null; children: React.ReactNode }) {
  if (snapshot === null)
    return <div className="flex h-12 shrink-0 items-center justify-end border-t border-line px-3">{children}</div>
  const indexes = snapshot.venues.filter((v) => v.index)
  const mean = indexes.length === 0 ? null : indexes.reduce((a, v) => a + Number(v.index), 0) / indexes.length
  return (
    <div className="flex h-12 shrink-0 items-center gap-7 border-t border-line pr-3 pl-4 text-sm">
      <Fact
        label="Forward"
        info={
          snapshot.forward_source === 'parity'
            ? 'The price the market expects at expiry, implied from put-call parity: a call minus a put at the same strike, near the money.'
            : `The price the market expects at expiry, taken from the ${snapshot.forward_source}.`
        }
      >
        {snapshot.forward ? fixed(Number(snapshot.forward), 1) : '—'}
      </Fact>
      <Fact label="At the money">
        {snapshot.atm_strike ? Number(snapshot.atm_strike).toLocaleString('en-US') : '—'}
      </Fact>
      <Fact label="Expires in">{lasting(snapshot.seconds_to_expiry / 86_400)}</Fact>
      {mean !== null && (
        <Fact
          label="Index"
          info={
            <span className="flex flex-col gap-1">
              <span>The rate each venue turns a coin premium into dollars at.</span>
              {indexes.map((v) => (
                <span key={v.venue} className="flex justify-between gap-4">
                  <span>{venueLabel(v.venue)}</span>
                  <span className="mono text-fg">{fixed(Number(v.index), 1)}</span>
                </span>
              ))}
            </span>
          }
        >
          {fixed(mean, 1)}
        </Fact>
      )}
      <span className="ml-auto">{children}</span>
    </div>
  )
}

const k_quick_expiries = 8

function ChainHeader({
  list,
  underlying,
  onUnderlying,
  expiry,
  onExpiry,
}: {
  list: OptionUnderlying[]
  underlying: OptionUnderlying | null
  onUnderlying: (u: string) => void
  expiry: string | null
  onExpiry: (e: string) => void
}) {
  const count = (u: OptionUnderlying) => u.venues.reduce((a, v) => a + v.options, 0)
  // the most widely listed first: those are the deep ones
  const sorted = [...list].sort(
    (a, b) => b.venues.length - a.venues.length || count(b) - count(a) || a.underlying.localeCompare(b.underlying),
  )
  const expiries = underlying?.expiries ?? []
  const quick = expiries.slice(0, k_quick_expiries)
  return (
    <div data-drag-handle className="flex shrink-0 flex-wrap items-center gap-2 px-3 py-2">
      <Picker
        label={
          <>
            {underlying !== null && <CoinIcon symbol={underlying.underlying} className="size-5" />}
            <span className="font-medium">{underlying?.underlying ?? 'Underlying'}</span>
          </>
        }
        search
        width={240}
        items={sorted.map((u) => ({
          key: u.underlying,
          text: u.underlying,
          label: (
            <>
              <CoinIcon symbol={u.underlying} className="size-5" />
              {u.underlying}
            </>
          ),
          hint: `${u.venues.length} ${u.venues.length === 1 ? 'venue' : 'venues'}`,
        }))}
        selected={underlying === null ? [] : [underlying.underlying]}
        onPick={onUnderlying}
      />
      <div className="flex min-w-0 items-center gap-1 overflow-x-auto" role="tablist" aria-label="Expiry">
        {quick.map((e) => (
          <button
            key={e.expiry}
            type="button"
            role="tab"
            aria-selected={e.expiry === expiry}
            title={`${e.venues.length} ${e.venues.length === 1 ? 'venue' : 'venues'}`}
            onClick={() => onExpiry(e.expiry)}
            className={cn(
              'flex h-8 shrink-0 items-center gap-1.5 rounded-md px-2.5 text-sm whitespace-nowrap transition-colors',
              e.expiry === expiry ? 'bg-active text-fg' : 'text-fg-mid hover:bg-hover hover:text-fg',
            )}
          >
            {expiryLabel(e.date)}
            <span className="mono text-xs text-fg-dim">{shortLasting(daysTo(e.date))}</span>
          </button>
        ))}
      </div>
      {expiries.length > quick.length && (
        <Picker
          label={
            quick.some((e) => e.expiry === expiry) || expiry === null ? (
              'Later'
            ) : (
              <span className="text-fg">{expiryLabel(expiries.find((e) => e.expiry === expiry)?.date ?? '')}</span>
            )
          }
          width={220}
          items={expiries.map((e) => ({
            key: e.expiry,
            label: expiryLabel(e.date),
            hint: shortLasting(daysTo(e.date)),
            group: monthOf(e.date),
          }))}
          selected={expiry === null ? [] : [expiry]}
          onPick={onExpiry}
        />
      )}
    </div>
  )
}

function ChainControls({
  underlying,
  venues,
  onVenues,
  native,
  onNative,
  expandAll,
  onExpandAll,
  columns,
  streamDown,
}: {
  underlying: OptionUnderlying | null
  venues: Set<string>
  onVenues: (v: Set<string>) => void
  native: boolean
  onNative: (n: boolean) => void
  expandAll: boolean
  onExpandAll: (e: boolean) => void
  columns: { shown: Set<Column>; toggle: (c: Column) => void }
  streamDown: boolean
}) {
  const offered = underlying?.venues ?? []
  const one = venues.size === 1 ? [...venues][0]! : null
  return (
    <div className="flex items-center gap-2">
      <Picker
        multi
        align="right"
        width={280}
        label={
          one !== null ? (
            <>
              <ExchangeIcon venue={one} className="size-4" />
              {venueLabel(one)}
            </>
          ) : venues.size === 0 ? (
            'All venues'
          ) : (
            `${venues.size} venues`
          )
        }
        items={offered.map((v) => ({
          key: v.venue,
          text: venueLabel(v.venue),
          label: (
            <>
              <ExchangeIcon venue={v.venue} className="size-4" />
              {venueLabel(v.venue)}
            </>
          ),
          hint: `${v.options.toLocaleString('en-US')} options`,
        }))}
        selected={[...venues]}
        onPick={(venue) => {
          const next = new Set(venues)
          if (next.has(venue)) next.delete(venue)
          else next.add(venue)
          onVenues(next)
        }}
        footer="With more than one venue, every bid and ask is the best among them, and the logo beside it says which venue quotes it."
      />
      <Seg
        size="md"
        options={[
          ['usd', 'USD'],
          ['coin', 'Coin'],
        ]}
        value={native ? 'coin' : 'usd'}
        onChange={(v) => onNative(v === 'coin')}
      />
      <button
        type="button"
        aria-pressed={expandAll}
        onClick={() => onExpandAll(!expandAll)}
        className={cn(
          'h-8 rounded-md px-3 text-sm transition-colors',
          expandAll ? 'bg-active text-fg' : 'bg-raised/60 text-fg-mid hover:bg-hover hover:text-fg',
        )}
      >
        Every venue
      </button>
      <Picker
        multi
        align="right"
        width={200}
        label="Columns"
        items={k_columns.map((c) => ({ key: c.key, label: c.label }))}
        selected={[...columns.shown]}
        onPick={columns.toggle}
      />{' '}
      {streamDown && <span className="text-sm text-amber">Stream down</span>}
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
  // Coin puts every venue in the coin: a dollar-premium venue converts at its own index
  const unit = native ? 'base' : 'quote'
  const priced = (v: number | null, usdV: number | null) =>
    !native ? usdV : leg.unit === 'base' ? v : usdV === null || leg.rate === null ? null : usdV / leg.rate
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
        <td className={cn('num px-2 text-right text-fg', shade && 'bg-raised/25')} title={tip(leg.mark, leg.markUsd)}>
          {v === null ? dash : money(v, unit)}
        </td>
      )
    }
    case 'bidIv':
    case 'askIv': {
      const v = column === 'bidIv' ? leg.bidIv : leg.askIv
      return (
        <td
          className={cn('num px-2 text-right text-fg-mid', shade && 'bg-raised/25', model(leg.ivModel))}
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
        v === null ? null : column === 'delta' ? v.toFixed(3) : column === 'gamma' ? v.toPrecision(3) : fixed(v, 2)
      return (
        <td
          className={cn('num px-2 text-right text-fg-mid', shade && 'bg-raised/25', model(leg.greeksModel))}
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
  return (
    <span className="ml-1.5 inline-flex w-3.5 align-[-2px]" title={venue === undefined ? undefined : venueLabel(venue)}>
      {venue !== undefined && <ExchangeIcon venue={venue} className="size-3.5" />}
    </span>
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
    <table
      className="w-full table-fixed border-collapse text-sm whitespace-nowrap"
      style={{ minWidth: [...calls, ...puts].reduce((a, c) => a + k_width[c], k_strike_width) }}
    >
      <colgroup>
        {calls.map((c) => (
          <col key={`c${c}`} style={{ width: k_width[c] }} />
        ))}
        <col style={{ width: k_strike_width }} />
        {puts.map((c) => (
          <col key={`p${c}`} style={{ width: k_width[c] }} />
        ))}
      </colgroup>
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-8">
          <th colSpan={calls.length} className="px-2 text-center text-sm font-medium text-fg-mid">
            Calls
          </th>
          <th className="px-2 text-center">
            <Info width={320}>
              <span className="flex flex-col gap-1.5">
                <span>
                  Each bid and ask is the best among the venues you chose, and the logo beside it says which venue
                  quotes it. Pick one venue to see only its prices, or click a row to see every venue.
                </span>
                <span>
                  A green figure beside a strike means one venue bids above another venue's ask for the same option,
                  before fees.
                </span>
              </span>
            </Info>
          </th>
          <th colSpan={puts.length} className="px-2 text-center text-sm font-medium text-fg-mid">
            Puts
          </th>
        </tr>
        <tr className="h-7 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          {calls.map((c) => (
            <th key={`c${c}`} className="col-head px-2 text-right">
              {label(c)}
            </th>
          ))}
          <th className="col-head bg-raised/60 px-3 text-center!">Strike</th>
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
          const venueNames = [...new Set([...row.call.legs, ...row.put.legs].map((l) => l.venue))].sort()
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
                    <span className="relative mx-auto block w-fit rounded-sm bg-panel px-2 text-xs text-accent">
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
                    best={c === 'bid' ? row.call.bestBid?.venue : c === 'ask' ? row.call.bestAsk?.venue : undefined}
                    multi={venueCount > 1}
                    coin={native ? forward : null}
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
                  <span className="relative inline-block">
                    {callCross !== null && (
                      <span className="absolute top-0 right-full mr-2 text-xs text-up">
                        +{fixed(callCross.pct, 1)}%
                      </span>
                    )}
                    {row.strike.toLocaleString('en-US')}
                    {putCross !== null && (
                      <span className="absolute top-0 left-full ml-2 text-xs text-up">+{fixed(putCross.pct, 1)}%</span>
                    )}
                  </span>
                </td>
                {puts.map((c) => (
                  <SummaryCell
                    key={`p${c}`}
                    leg={put}
                    column={c}
                    itm={itmPut}
                    best={c === 'bid' ? row.put.bestBid?.venue : c === 'ask' ? row.put.bestAsk?.venue : undefined}
                    multi={venueCount > 1}
                    coin={native ? forward : null}
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
  coin,
}: {
  leg: Leg
  column: Column
  itm: boolean
  best: string | undefined
  multi: boolean
  /** the forward when premiums read in the coin: the best of several venues is in dollars, so it converts here */
  coin: number | null
}) {
  const priced = column === 'bid' || column === 'ask' || column === 'mark'
  if (!priced) return <Cell leg={leg} column={column} native={false} shade={itm} />
  const usdValue = column === 'bid' ? leg.bidUsd : column === 'ask' ? leg.askUsd : leg.markUsd
  const value = usdValue === null ? null : coin === null ? usdValue : usdValue / coin
  const tone = column === 'bid' ? 'text-up' : column === 'ask' ? 'text-down' : 'text-fg'
  // the summary is the best across venues, so its quote carries the venue that set it
  const mark = column !== 'mark' && multi && best !== undefined
  return (
    <td
      className={cn('px-2 text-right', itm && 'bg-raised/25')}
      title={mark ? `Best ${column} on ${venueLabel(best)}` : undefined}
    >
      {value === null ? (
        <span className="text-fg-dim">—</span>
      ) : (
        <>
          <span className={cn('num', tone)}>{money(value, coin === null ? 'quote' : 'base')}</span>
          {mark && <BestVenue venue={best} />}
        </>
      )}
    </td>
  )
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
      byVenue.set(p.venue, [...(byVenue.get(p.venue) ?? []), { x: Number(p.strike), y: Number(p.mid_iv) * 100 }])
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
        label: 'Fit',
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
      title={slice === null ? 'Smile' : `Smile, ${expiryLabel(slice.date)}`}
      info="Implied volatility across strikes for this expiry, from out-of-the-money options. Dots are venue quotes and the line is a smooth fit. A dot well above the line is expensive, one below it is cheap; on thin strikes check the bid and ask first."
      legend={series.map((s) => ({ key: s.key, label: s.label, color: s.color }))}
      onLegendHover={setHighlight}
      readout={
        slice === null ? undefined : (
          <div className="flex flex-wrap gap-x-4 gap-y-0.5 text-xs text-fg-dim">
            <span>
              At the money <span className="mono text-fg">{pct(num(slice.atm_iv)) ?? '—'}%</span>
            </span>
            <span>
              Risk reversal <span className="mono text-fg">{pct(num(slice.rr25), 1) ?? '—'}</span>
            </span>
            <span>
              Butterfly <span className="mono text-fg">{pct(num(slice.bf25), 1) ?? '—'}</span>
            </span>
          </div>
        )
      }
      height={260}
      placeholder={
        series.length === 0 ? (surface === null ? 'No surface yet' : 'Nothing quoted on this expiry') : undefined
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
      title="Term structure"
      info="At-the-money implied volatility for each expiry, by days to expiry. It normally slopes up; when near expiries sit above far ones, the market is pricing stress now. Expiries off the line are rich or cheap against their neighbours."
      height={200}
      placeholder={points.length === 0 ? 'No surface yet' : undefined}
    >
      {({ width, height }) => (
        <XYPlot
          series={[{ key: 'atm', label: 'At-the-money volatility', color: '#4c9aff', points, mode: 'both' }]}
          width={width}
          height={height}
          formatX={(x) => `${x.toFixed(0)}d`}
          formatY={(y) => `${y.toFixed(1)}%`}
          markers={chosen === null ? [] : [{ x: chosen, label: 'this expiry' }]}
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
  const both = [series('rr25', 'Risk reversal', '#a78bfa'), series('bf25', 'Butterfly', '#f59e0b')]
  return (
    <ChartFrame
      title="Skew"
      info={
        <span className="flex flex-col gap-1.5">
          <span>
            Both in volatility points, by days to expiry, measured at 25 delta (options about one in four likely to
            finish in the money).
          </span>
          <span>
            Risk reversal: call volatility minus put volatility. Below zero, puts cost more and traders are paying for
            downside protection; above zero, upside calls are in demand.
          </span>
          <span>
            Butterfly: the average of call and put volatility minus at-the-money volatility. Higher means large moves
            either way are priced richer than small ones.
          </span>
        </span>
      }
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
