import { useEffect, useMemo, useRef, useState } from 'react'
import { Icon } from '@/components/shared/Icon'
import { SortHead, type Sort } from '@/components/shared/SortHead'
import { Venue } from '@/components/shared/Venue'
import { cn } from '@/lib/utils'
import {
  FUNDING_BOARD,
  SCANNER_VENUES,
  SPOT_BOARD,
  SPOT_PERP_BOARD,
  type FundingRow,
  type SpotPerpRow,
  type SpotRow,
} from '@/mock/scanner'

const TABS = [
  { key: 'perp-perp', label: 'Perp · Perp' },
  { key: 'spot-perp', label: 'Spot · Perp' },
  { key: 'spot-spot', label: 'Spot · Spot' },
] as const
type Tab = (typeof TABS)[number]['key']

const num = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })
const pct = (v: number, dp = 4) => `${v >= 0 ? '+' : '−'}${Math.abs(v).toFixed(dp)}%`
const rate = (v: number) => pct(v * 100)
/** The crossing cost, coloured by how much of the edge it eats.
 *
 *  A BBO spread is never negative, so a sign has nothing to say about it and
 *  the column sat grey while everything around it carried meaning. What it
 *  actually tells you is whether the route survives being taken: under two
 *  basis points a book is tight enough to ignore, past eight it is the trade.
 *  The steps are a judgement, and they are the judgement this column exists
 *  to support. */
const bboTone = (pct: number) =>
  pct >= 0.08 ? 'text-down' : pct >= 0.04 ? 'text-amber' : 'text-fg'

/** Premium is signed, so it reads like every other signed column on the
 *  screen: over is up, under is down. */
const signTone = (v: number) => (v >= 0 ? 'text-up' : 'text-down')

const compact = (v: number) =>
  v >= 1e9 ? `${(v / 1e9).toFixed(1)}B` : v >= 1e6 ? `${(v / 1e6).toFixed(0)}M` : `${(v / 1e3).toFixed(0)}K`

/** The next settlement, as time remaining rather than a wall clock.
 *
 *  A board that prints `08:00` makes the reader work out how far away that is,
 *  in UTC, per venue, on a row whose two legs settle on different schedules.
 *  What they are deciding is whether there is time to get in before it pays. */
const nextAt = (intervalH: number, now: number) => {
  const ms = intervalH * 3_600_000
  return Math.ceil(now / ms) * ms
}
const remaining = (target: number, now: number) => {
  const s = Math.max(0, Math.round((target - now) / 1000))
  return `${Math.floor(s / 3600)}:${String(Math.floor((s % 3600) / 60)).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`
}

function useNow(ms = 1000) {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms)
    return () => clearInterval(id)
  }, [ms])
  return now
}

function useSorted<T, K extends string>(
  rows: T[],
  sort: Sort<K>,
  value: (r: T, k: K) => number | string,
) {
  return useMemo(() => {
    return [...rows].sort((a, b) => {
      const x = value(a, sort.key)
      const y = value(b, sort.key)
      const c = typeof x === 'string' ? x.localeCompare(y as string) : x - (y as number)
      return sort.desc ? -c : c
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [rows, sort])
}

/** Three boards, one question each.
 *
 *  perp·perp is a funding carry: hold the leg that pays and short the leg that
 *  charges. spot·perp is a basis: hold the coin and short the contract.
 *  spot·spot is a straight arbitrage.
 *
 *  Every row names BOTH venues, in two columns rather than one route cell —
 *  the columns are sorted and compared, and a pair crammed into one cell can
 *  be neither. */
export function ScannerPage() {
  const [tab, setTab] = useState<Tab>('perp-perp')
  const [search, setSearch] = useState('')
  const [minEdge, setMinEdge] = useState(0)
  const [minVolume, setMinVolume] = useState(0)
  const [excluded, setExcluded] = useState<Set<string>>(() => new Set())
  const now = useNow()

  const on = (v: string) => !excluded.has(v)
  const matches = (symbol: string) => symbol.toLowerCase().includes(search.toLowerCase())
  const bigEnough = (v: number) => v >= minVolume * 1e6

  const funding = FUNDING_BOARD.filter(
    (r) =>
      on(r.longVenue) &&
      on(r.shortVenue) &&
      Math.abs(r.fundingSpreadPct * 100) >= minEdge &&
      bigEnough(r.volume24hMinUsd) &&
      matches(r.symbol),
  )
  const spotPerp = SPOT_PERP_BOARD.filter(
    (r) =>
      on(r.spotVenue) &&
      on(r.perpVenue) &&
      Math.abs(r.basisBps) >= minEdge &&
      bigEnough(r.volume24hMinUsd) &&
      matches(r.symbol),
  )
  const spot = SPOT_BOARD.filter(
    (r) =>
      on(r.buyVenue) &&
      on(r.sellVenue) &&
      r.spreadBps >= minEdge &&
      bigEnough(r.volume24hMinUsd) &&
      matches(r.symbol),
  )
  const rows =
    tab === 'perp-perp' ? funding.length : tab === 'spot-perp' ? spotPerp.length : spot.length

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex shrink-0 flex-wrap items-center gap-x-4 gap-y-2 px-3 py-2">
        <div className="flex gap-4">
          {TABS.map((t) => (
            <button
              key={t.key}
              type="button"
              onClick={() => setTab(t.key)}
              className={cn(
                'relative h-6 text-xs transition-colors duration-150',
                tab === t.key
                  ? 'font-medium text-fg after:absolute after:inset-x-0 after:-bottom-1 after:h-[2px] after:rounded-t-full after:bg-accent'
                  : 'text-fg-dim hover:text-fg-mid',
              )}
            >
              {t.label}
            </button>
          ))}
        </div>

        <span className="h-4 w-px bg-line" />
        <VenuePicker excluded={excluded} onChange={setExcluded} />
        <span className="h-4 w-px bg-line" />

        <NumberField label="Min edge" unit="bp" value={minEdge} step={0.5} onChange={setMinEdge} />
        <NumberField label="Min volume" unit="M" value={minVolume} step={5} onChange={setMinVolume} />

        <div className="relative ml-auto w-52">
          <Icon.Search
            className="pointer-events-none absolute top-1/2 left-2 size-3 -translate-y-1/2 text-fg-dim"
            strokeWidth={1.75}
          />
          <input
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder="Filter symbol"
            className="h-7 w-full rounded-sm bg-raised pr-2.5 pl-7 text-xs text-fg outline-none transition-shadow duration-150 placeholder:text-fg-dim focus:shadow-[inset_0_0_0_1px_color-mix(in_srgb,var(--color-accent)_55%,transparent)]"
          />
        </div>
        <span className="meta w-20 text-right">{rows} routes</span>
      </section>

      <section className="panel flex min-h-0 flex-1 flex-col">
        <div className="min-h-0 flex-1 overflow-auto">
          {tab === 'perp-perp' && <FundingTable rows={funding} now={now} />}
          {tab === 'spot-perp' && <SpotPerpTable rows={spotPerp} now={now} />}
          {tab === 'spot-spot' && <SpotTable rows={spot} />}
        </div>
      </section>
    </div>
  )
}

/** Twenty venues do not fit on a filter row, and a row that wraps onto three
 *  lines pushes the board it filters off the screen. Excluded, not included:
 *  a desk trades everything it is keyed for until it has a reason not to. */
function VenuePicker({
  excluded,
  onChange,
}: {
  excluded: Set<string>
  onChange: (s: Set<string>) => void
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

  const toggle = (v: string) => {
    const next = new Set(excluded)
    if (next.has(v)) next.delete(v)
    else next.add(v)
    onChange(next)
  }
  const kept = SCANNER_VENUES.length - excluded.size

  return (
    <div ref={box} className="relative">
      <button
        type="button"
        onClick={() => setOpen(!open)}
        className={cn(
          'flex h-7 items-center gap-2 rounded-sm px-2.5 text-xs transition-colors duration-150',
          open ? 'bg-active text-fg' : 'bg-raised text-fg-mid hover:bg-hover hover:text-fg',
        )}
      >
        Venues
        <span className="num text-2xs text-fg-dim">
          {kept}/{SCANNER_VENUES.length}
        </span>
        <Icon.Caret
          className={cn('size-3.5 text-fg-dim transition-transform duration-150', open && 'rotate-180')}
          strokeWidth={1.75}
        />
      </button>

      {open && (
        <div className="absolute top-8 left-0 z-30 w-56 rounded-md bg-raised p-1 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.8)]">
          <div className="flex gap-1 px-1 pb-1">
            <button
              type="button"
              onClick={() => onChange(new Set())}
              className="h-6 flex-1 rounded-sm bg-panel text-2xs text-fg-mid transition-colors duration-150 hover:text-fg"
            >
              All
            </button>
            <button
              type="button"
              onClick={() => onChange(new Set(SCANNER_VENUES))}
              className="h-6 flex-1 rounded-sm bg-panel text-2xs text-fg-mid transition-colors duration-150 hover:text-fg"
            >
              None
            </button>
          </div>
          <div className="max-h-72 overflow-auto">
            {SCANNER_VENUES.map((v) => {
              const kept = !excluded.has(v)
              return (
                <button
                  key={v}
                  type="button"
                  onClick={() => toggle(v)}
                  className="flex h-7 w-full items-center gap-2 rounded-sm px-1.5 text-left transition-colors duration-150 hover:bg-hover"
                >
                  <span
                    className={cn(
                      'flex size-3.5 shrink-0 items-center justify-center rounded-[3px] text-[9px]',
                      kept ? 'bg-accent text-canvas' : 'bg-panel text-transparent',
                    )}
                  >
                    ✓
                  </span>
                  <Venue venue={v} />
                </button>
              )
            })}
          </div>
        </div>
      )}
    </div>
  )
}

function NumberField({
  label,
  unit,
  value,
  step,
  onChange,
}: {
  label: string
  unit: string
  value: number
  step: number
  onChange: (v: number) => void
}) {
  return (
    <label className="flex items-center gap-1.5 text-2xs text-fg-dim">
      {label}
      <input
        type="number"
        step={step}
        value={value}
        onChange={(e) => onChange(globalThis.Number(e.target.value) || 0)}
        className="num h-6 w-16 rounded-sm bg-raised px-2 text-right text-xs text-fg outline-none"
      />
      {unit}
    </label>
  )
}

const Head = ({ children }: { children: React.ReactNode }) => (
  <thead className="sticky top-0 z-10 bg-panel">
    <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">{children}</tr>
  </thead>
)
const Th = ({ children, right }: { children: React.ReactNode; right?: boolean }) => (
  <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left')}>{children}</th>
)
const Pred = ({ value }: { value: number | null }) => (
  <td className="num px-3 text-right text-fg-mid">
    {value === null ? <span className="text-fg-dim">—</span> : rate(value)}
  </td>
)
const Count = ({ intervalH, now }: { intervalH: number; now: number }) => (
  <td className="num px-3 text-right text-fg-mid">{remaining(nextAt(intervalH, now), now)}</td>
)

type FundKey =
  | 'symbol' | 'longVenue' | 'shortVenue' | 'rateLong' | 'rateShort'
  | 'fundingSpreadPct' | 'basisBps' | 'bboSpreadPct' | 'premiumPct' | 'volume24hMinUsd'

function FundingTable({ rows, now }: { rows: FundingRow[]; now: number }) {
  const [sort, setSort] = useState<Sort<FundKey>>({ key: 'fundingSpreadPct', desc: true })
  const sorted = useSorted(rows, sort, (r, k) => r[k])

  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <Head>
        <SortHead k="symbol" sort={sort} onSort={setSort}>Symbol</SortHead>
        <SortHead k="longVenue" sort={sort} onSort={setSort}>Long</SortHead>
        <SortHead k="shortVenue" sort={sort} onSort={setSort}>Short</SortHead>
        <SortHead k="rateLong" sort={sort} onSort={setSort} right>Rate L</SortHead>
        <SortHead k="rateShort" sort={sort} onSort={setSort} right>Rate S</SortHead>
        <Th right>Pred L</Th>
        <Th right>Pred S</Th>
        <Th right>Int L</Th>
        <Th right>Int S</Th>
        <SortHead k="fundingSpreadPct" sort={sort} onSort={setSort} right>Spread FR</SortHead>
        <SortHead k="basisBps" sort={sort} onSort={setSort} right>Spread bp</SortHead>
        <SortHead k="bboSpreadPct" sort={sort} onSort={setSort} right>BBO</SortHead>
        <SortHead k="premiumPct" sort={sort} onSort={setSort} right>Premium</SortHead>
        <SortHead k="volume24hMinUsd" sort={sort} onSort={setSort} right>Vol 24h</SortHead>
        <Th right>Next L</Th>
        <Th right>Next S</Th>
      </Head>
      <tbody>
        {sorted.map((r) => (
          <tr key={r.symbol} className="row">
            <td className="px-3 text-fg">{r.symbol}</td>
            <td className="px-3">
              <Venue venue={r.longVenue} />
            </td>
            <td className="px-3">
              <Venue venue={r.shortVenue} />
            </td>
            <td className={cn('num px-3 text-right', r.rateLong < 0 ? 'text-down' : 'text-fg')}>
              {rate(r.rateLong)}
            </td>
            <td className={cn('num px-3 text-right', r.rateShort < 0 ? 'text-down' : 'text-fg')}>
              {rate(r.rateShort)}
            </td>
            <Pred value={r.predictedLong} />
            <Pred value={r.predictedShort} />
            {/* One interval per venue. `4/8h` in a single cell is two facts
                pretending to be one, and it cannot be sorted or compared. */}
            <td className="num px-3 text-right text-fg-dim">{r.intervalLongH}h</td>
            <td className="num px-3 text-right text-fg-dim">{r.intervalShortH}h</td>
            {/* What the trade earns per period... */}
            <td
              className={cn(
                'num px-3 text-right font-medium',
                r.fundingSpreadPct >= 0 ? 'text-up' : 'text-down',
              )}
            >
              {pct(r.fundingSpreadPct)}
            </td>
            {/* ...and how far apart the two venues quote it, which is what it
                costs to get in and what has to converge to get out. */}
            <td className={cn('num px-3 text-right', r.basisBps >= 0 ? 'text-fg' : 'text-down')}>
              {num(r.basisBps, 1)}
            </td>
            <td className={cn('num px-3 text-right', bboTone(r.bboSpreadPct))}>
              {r.bboSpreadPct.toFixed(3)}%
            </td>
            <td className={cn('num px-3 text-right', signTone(r.premiumPct))}>
              {pct(r.premiumPct, 3)}
            </td>
            <td className="num px-3 text-right text-fg-mid">{compact(r.volume24hMinUsd)}</td>
            <Count intervalH={r.intervalLongH} now={now} />
            <Count intervalH={r.intervalShortH} now={now} />
          </tr>
        ))}
      </tbody>
    </table>
  )
}

type BasisKey =
  | 'symbol' | 'spotVenue' | 'perpVenue' | 'spotAsk' | 'perpBid'
  | 'basisBps' | 'perpRate' | 'premiumPct' | 'volume24hMinUsd'

function SpotPerpTable({ rows, now }: { rows: SpotPerpRow[]; now: number }) {
  const [sort, setSort] = useState<Sort<BasisKey>>({ key: 'basisBps', desc: true })
  const sorted = useSorted(rows, sort, (r, k) => r[k])

  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <Head>
        <SortHead k="symbol" sort={sort} onSort={setSort}>Symbol</SortHead>
        <SortHead k="spotVenue" sort={sort} onSort={setSort}>Spot</SortHead>
        <SortHead k="perpVenue" sort={sort} onSort={setSort}>Perp</SortHead>
        <SortHead k="spotAsk" sort={sort} onSort={setSort} right>Spot ask</SortHead>
        <SortHead k="perpBid" sort={sort} onSort={setSort} right>Perp bid</SortHead>
        <SortHead k="basisBps" sort={sort} onSort={setSort} right>Spread bp</SortHead>
        <SortHead k="perpRate" sort={sort} onSort={setSort} right>Funding</SortHead>
        <Th right>Predicted</Th>
        <Th right>Int</Th>
        <SortHead k="premiumPct" sort={sort} onSort={setSort} right>Premium</SortHead>
        <SortHead k="volume24hMinUsd" sort={sort} onSort={setSort} right>Vol 24h</SortHead>
        <Th right>Next</Th>
      </Head>
      <tbody>
        {sorted.map((r) => (
          <tr key={r.symbol} className="row">
            <td className="px-3 text-fg">{r.symbol}</td>
            <td className="px-3">
              <Venue venue={r.spotVenue} />
            </td>
            <td className="px-3">
              <Venue venue={r.perpVenue} />
            </td>
            <td className="num px-3 text-right text-up">{num(r.spotAsk, 3)}</td>
            <td className="num px-3 text-right text-down">{num(r.perpBid, 3)}</td>
            <td
              className={cn('num px-3 text-right font-medium', r.basisBps >= 0 ? 'text-up' : 'text-down')}
            >
              {num(r.basisBps, 1)}
            </td>
            <td className={cn('num px-3 text-right', r.perpRate < 0 ? 'text-down' : 'text-fg')}>
              {rate(r.perpRate)}
            </td>
            <Pred value={r.predictedRate} />
            <td className="num px-3 text-right text-fg-dim">{r.intervalH}h</td>
            <td className={cn('num px-3 text-right', signTone(r.premiumPct))}>
              {pct(r.premiumPct, 3)}
            </td>
            <td className="num px-3 text-right text-fg-mid">{compact(r.volume24hMinUsd)}</td>
            <Count intervalH={r.intervalH} now={now} />
          </tr>
        ))}
      </tbody>
    </table>
  )
}

type SpotKey = 'symbol' | 'buyVenue' | 'sellVenue' | 'buyAsk' | 'sellBid' | 'spreadBps' | 'volume24hMinUsd'

function SpotTable({ rows }: { rows: SpotRow[] }) {
  const [sort, setSort] = useState<Sort<SpotKey>>({ key: 'spreadBps', desc: true })
  const sorted = useSorted(rows, sort, (r, k) => r[k])

  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <Head>
        <SortHead k="symbol" sort={sort} onSort={setSort}>Symbol</SortHead>
        <SortHead k="buyVenue" sort={sort} onSort={setSort}>Buy</SortHead>
        <SortHead k="sellVenue" sort={sort} onSort={setSort}>Sell</SortHead>
        <SortHead k="buyAsk" sort={sort} onSort={setSort} right>Buy at ask</SortHead>
        <SortHead k="sellBid" sort={sort} onSort={setSort} right>Sell at bid</SortHead>
        <SortHead k="spreadBps" sort={sort} onSort={setSort} right>Spread bp</SortHead>
        <SortHead k="volume24hMinUsd" sort={sort} onSort={setSort} right>Vol 24h</SortHead>
      </Head>
      <tbody>
        {sorted.map((r) => (
          <tr key={r.symbol} className="row">
            <td className="px-3 text-fg">{r.symbol}</td>
            <td className="px-3">
              <Venue venue={r.buyVenue} />
            </td>
            <td className="px-3">
              <Venue venue={r.sellVenue} />
            </td>
            <td className="num px-3 text-right text-up">{num(r.buyAsk, 3)}</td>
            <td className="num px-3 text-right text-down">{num(r.sellBid, 3)}</td>
            <td
              className={cn('num px-3 text-right font-medium', r.spreadBps >= 0 ? 'text-up' : 'text-down')}
            >
              {num(r.spreadBps, 1)}
            </td>
            <td className="num px-3 text-right text-fg-mid">{compact(r.volume24hMinUsd)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}
