import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { cn } from '@raikiri/ui'
import { n } from '@raikiri/ui/v3'
import { api, type Funding, type MetricPage } from '../api/client'
import { bp, px, type Leg } from '../lib/market'
import { requestMarketTab } from './market-tab-signal'

const k_open_ms = 120
const k_close_ms = 220
const k_width = 248
const k_hour_ms = 3_600_000
const k_day_ms = 24 * k_hour_ms
const k_year_s = 8_760 * 3_600
const k_average_ms = 7 * k_day_ms
const k_fresh_ms = 60_000

const k_history = new Map<string, { at: number; page: Promise<MetricPage> }>()

function historyOf(id: string) {
  const kept = k_history.get(id)
  if (kept !== undefined && Date.now() - kept.at < k_fresh_ms) return kept.page
  const page = api.metricsHistory(id, '1h', 500)
  k_history.set(id, { at: Date.now(), page })
  page.catch(() => k_history.get(id)?.page === page && k_history.delete(id))
  return page
}

/** the settled rates behind the live one: the last, and their mean over the week as far as history reaches */
function settledOf(page: MetricPage, intervalS: number, next: number | undefined, now: number) {
  const span = intervalS * 1000
  const rates = new Map<number, number>()
  for (const p of page.points) {
    const r = n(p.funding_rate)
    if (r !== undefined) rates.set(p.t, r)
  }
  const last = next === undefined ? Math.floor(now / span) * span : next - Math.ceil((next - now) / span) * span
  // the hour before a funding time ends on the rate that settled; a backfilled print sits in the hour of the time
  const at = (time: number) => {
    const hour = Math.floor(time / k_hour_ms) * k_hour_ms
    return rates.get(hour - k_hour_ms) ?? rates.get(hour)
  }
  let sum = 0
  let count = 0
  let newest: number | undefined
  let oldest: number | undefined
  for (let time = last; time > now - k_average_ms; time -= span) {
    const r = at(time)
    if (r === undefined) continue
    sum += r
    count += 1
    newest ??= time
    oldest = time
  }
  const covered = newest === undefined || oldest === undefined ? 0 : newest - oldest + span
  return {
    previous: at(last),
    average: covered >= k_day_ms ? { rate: sum / count, days: Math.round(covered / k_day_ms) } : undefined,
  }
}

const percent = (ratio: number, places: number) => `${(ratio * 100).toFixed(places)}%`
const toneOf = (v: number) => (v > 0 ? 'text-up' : v < 0 ? 'text-down' : 'text-fg')

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-baseline justify-between gap-4 py-0.5">
      <span className="text-fg-dim">{label}</span>
      <span className="mono text-fg">{children}</span>
    </div>
  )
}

/** a rate per interval beside what it comes to over a year */
function Rate({ rate, perYear }: { rate: number; perYear: number | undefined }) {
  return (
    <span className="flex items-baseline gap-2">
      <span className={toneOf(rate)}>{percent(rate, 4)}</span>
      {perYear !== undefined && <span className="text-fg-mid">{percent(rate * perYear, 2)}</span>}
    </span>
  )
}

function Details({ leg, funding, onDone }: { leg: Leg; funding: Funding; onDone: () => void }) {
  const [page, setPage] = useState<MetricPage>()
  useEffect(() => {
    let live = true
    historyOf(leg.id).then(
      (p) => live && setPage(p),
      () => undefined,
    )
    return () => {
      live = false
    }
  }, [leg.id])
  const rate = n(funding.rate)
  const predicted = n(funding.predicted_rate)
  const mark = n(funding.mark_price)
  const index = n(funding.index_price)
  const seconds = funding.interval_seconds ?? undefined
  const perYear = seconds === undefined || seconds <= 0 ? undefined : k_year_s / seconds
  const next = funding.next_funding_time === undefined ? undefined : Number(funding.next_funding_time) / 1e6
  const settled =
    page === undefined || seconds === undefined || seconds <= 0
      ? undefined
      : settledOf(page, seconds, next, Date.now())
  return (
    <>
      {rate !== undefined && perYear !== undefined && (
        <Row label="Annualized">
          <span className={toneOf(rate)}>{percent(rate * perYear, 2)}</span>
        </Row>
      )}
      {rate !== undefined && rate !== 0 && (
        <Row label="Direction">
          <span className={rate > 0 ? 'text-up' : 'text-down'}>{rate > 0 ? 'Long' : 'Short'}</span>
          <span className="text-fg-mid"> pays </span>
          <span className={rate > 0 ? 'text-down' : 'text-up'}>{rate > 0 ? 'Short' : 'Long'}</span>
        </Row>
      )}
      {predicted !== undefined && (
        <Row label="Predicted">
          <Rate rate={predicted} perYear={perYear} />
        </Row>
      )}
      {settled?.previous !== undefined && (
        <Row label="Previous">
          <Rate rate={settled.previous} perYear={perYear} />
        </Row>
      )}
      {settled?.average !== undefined && perYear !== undefined && (
        <Row label={`${settled.average.days}d avg`}>
          <span className={toneOf(settled.average.rate)}>{percent(settled.average.rate * perYear, 2)}</span>
        </Row>
      )}
      {mark !== undefined && <Row label="Mark">{px(mark, leg.places)}</Row>}
      {index !== undefined && <Row label="Index">{px(index, leg.places)}</Row>}
      {mark !== undefined && index !== undefined && index > 0 && (
        <Row label="Basis">
          <span className={toneOf(mark - index)}>{bp(((mark - index) / index) * 1e4, 2)}</span>
        </Row>
      )}
      <button
        type="button"
        onClick={() => {
          requestMarketTab('data')
          onDone()
        }}
        className="mt-1.5 h-7 w-full rounded-sm bg-hover text-fg-mid transition-colors hover:bg-active hover:text-fg"
      >
        History
      </button>
    </>
  )
}

/** the funding tile: hovering or clicking it opens what the rate comes to, drawn over the page so the header's
 *  scroller cannot clip it */
export function FundingTile({ leg, funding, children }: { leg: Leg; funding: Funding; children: React.ReactNode }) {
  const [open, setOpen] = useState(false)
  const [at, setAt] = useState<{ top: number; left: number }>()
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined)
  const tile = useRef<HTMLButtonElement>(null)
  const panel = useRef<HTMLDivElement>(null)
  const later = (next: boolean, ms: number) => {
    clearTimeout(timer.current)
    timer.current = setTimeout(() => setOpen(next), ms)
  }
  useLayoutEffect(() => {
    if (!open) return
    const place = () => {
      const rect = tile.current?.getBoundingClientRect()
      if (rect === undefined) return
      setAt({
        top: rect.bottom + 6,
        left: Math.max(8, Math.min(rect.left, window.innerWidth - k_width - 8)),
      })
    }
    place()
    window.addEventListener('resize', place)
    window.addEventListener('scroll', place, true)
    return () => {
      window.removeEventListener('resize', place)
      window.removeEventListener('scroll', place, true)
    }
  }, [open])
  useEffect(() => {
    if (!open) return
    const inside = (target: EventTarget | null) =>
      target instanceof Node && ((tile.current?.contains(target) ?? false) || (panel.current?.contains(target) ?? false))
    const off = (e: MouseEvent) => !inside(e.target) && setOpen(false)
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setOpen(false)
    window.addEventListener('mousedown', off)
    window.addEventListener('keydown', esc)
    return () => {
      window.removeEventListener('mousedown', off)
      window.removeEventListener('keydown', esc)
    }
  }, [open])
  useEffect(() => () => clearTimeout(timer.current), [])
  return (
    <>
      <button
        ref={tile}
        type="button"
        aria-expanded={open}
        onMouseEnter={() => later(true, k_open_ms)}
        onMouseLeave={() => later(false, k_close_ms)}
        onClick={() => {
          clearTimeout(timer.current)
          setOpen((o) => !o)
        }}
        className={cn(
          '-mx-2 shrink-0 rounded-md px-2 py-1 text-left transition-colors hover:bg-hover',
          open && 'bg-hover',
        )}
      >
        {children}
      </button>
      {open &&
        at !== undefined &&
        createPortal(
          <div
            ref={panel}
            onMouseEnter={() => later(true, 0)}
            onMouseLeave={() => later(false, k_close_ms)}
            className="v3 v3-pop fixed z-50 rounded-md bg-raised px-3 py-2 text-xs shadow-[0_0_0_1px_var(--color-line),0_24px_60px_-12px_rgb(0_0_0/0.9)]"
            style={{ top: at.top, left: at.left, width: k_width }}
          >
            <Details leg={leg} funding={funding} onDone={() => setOpen(false)} />
          </div>,
          document.body,
        )}
    </>
  )
}
