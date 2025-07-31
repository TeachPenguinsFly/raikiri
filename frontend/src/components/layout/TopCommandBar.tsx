import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/utils'
import { useGateways } from '@/mock/data'

/** UTC, and said so.
 *
 *  A trading terminal that shows local time is a terminal where two people
 *  reading the same screen in two cities disagree about when a fill happened.
 *  Venues stamp in UTC and so does the journal. */
function Clock() {
  const [now, setNow] = useState(() => new Date())
  useEffect(() => {
    const id = setInterval(() => setNow(new Date()), 1000)
    return () => clearInterval(id)
  }, [])
  return (
    <div className="num flex items-baseline gap-1 text-xs text-fg-mid">
      <span>{now.toLocaleTimeString('en-GB', { hour12: false, timeZone: 'UTC' })}</span>
      <span className="text-2xs text-fg-dim">UTC</span>
    </div>
  )
}

/** Which gateways are answering, as a dot you can open.
 *
 *  Collapsed to a count because it is right most of the time and a header full
 *  of green dots trains you to stop reading it; expanded when the count is not
 *  the whole set, because THEN which one is missing is the question. */
function GatewayStatus() {
  const gateways = useGateways()
  const [open, setOpen] = useState(false)
  const ref = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => {
      if (ref.current !== null && !ref.current.contains(e.target as Node)) setOpen(false)
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [open])

  const alive = gateways.filter((g) => g.alive).length
  const all = gateways.length
  const tone = alive === all ? 'bg-up' : alive === 0 ? 'bg-down' : 'bg-amber'

  return (
    <div className="relative" ref={ref}>
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        className={cn(
          'flex h-8 items-center gap-2 rounded-md px-2.5 text-xs transition-colors duration-150',
          open ? 'bg-active text-fg' : 'text-fg-mid hover:bg-hover hover:text-fg',
        )}
      >
        <span className={cn('size-[5px] rounded-full', tone, alive < all && 'pulse-dot')} />
        <span className="num tabular-nums">
          {alive}/{all}
        </span>
        <span className="text-fg-dim">gateways</span>
      </button>

      {open && (
        <div className="absolute right-0 top-9 z-30 w-60 rounded-lg bg-raised p-1 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.7)]">
          {gateways.map((g) => (
            <div key={g.account} className="flex h-7 items-center gap-2 rounded-sm px-2 text-xs">
              <span className={cn('size-[5px] rounded-full', g.alive ? 'bg-up' : 'bg-down')} />
              <span className="text-fg">{g.account}</span>
              <span className="num ml-auto text-xs text-fg-dim">
                {g.alive ? g.instruments.toLocaleString() : '—'}
              </span>
            </div>
          ))}
          {gateways.length === 0 && (
            <div className="px-2 py-3 text-center text-xs text-fg-dim">No gateway is attached.</div>
          )}
        </div>
      )}
    </div>
  )
}

/** System status, and nothing else.
 *
 *  The bar used to open with a search field that did nothing — the working
 *  filter lives in the market list, where the thing being filtered is. A
 *  disabled control in the most prominent position on the page teaches the
 *  reader to distrust the row it sits in. */
export function TopCommandBar() {
  return (
    <header className="flex h-12 shrink-0 items-center justify-end gap-2 px-3">
      <GatewayStatus />
      <span className="h-4 w-px bg-line" />
      <Clock />
    </header>
  )
}
