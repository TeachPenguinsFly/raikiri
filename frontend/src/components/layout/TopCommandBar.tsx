import { useEffect, useRef, useState } from 'react'
import { Icon } from '@/components/shared/Icon'
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
    <div className="num flex items-baseline gap-1.5 text-[12px] text-fg-mid">
      <span>{now.toLocaleTimeString('en-GB', { hour12: false, timeZone: 'UTC' })}</span>
      <span className="text-3xs text-fg-dim">UTC</span>
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
          'flex h-7 items-center gap-2 rounded-lg px-2.5 text-2xs font-semibold transition-colors',
          open ? 'bg-white/8 text-fg' : 'text-fg-mid hover:bg-white/4 hover:text-fg',
        )}
      >
        <span className={cn('size-1.5 rounded-full', tone, alive < all && 'pulse-dot')} />
        <span className="num">
          {alive}/{all}
        </span>
        <span className="text-fg-dim">gateways</span>
      </button>

      {open && (
        <div className="absolute right-0 top-8 z-20 w-64 rounded-xl border border-line bg-panel p-1.5 shadow-2xl">
          {gateways.map((g) => (
            <div key={g.account} className="flex items-center gap-2 rounded-lg px-2 py-1.5 text-2xs">
              <span className={cn('size-1.5 rounded-full', g.alive ? 'bg-up' : 'bg-down')} />
              <span className="text-fg">{g.account}</span>
              <span className="num ml-auto text-fg-dim">
                {g.instruments.toLocaleString()}
              </span>
            </div>
          ))}
          {gateways.length === 0 && (
            <div className="px-2 py-3 text-center text-2xs text-fg-dim">
              No gateway is attached.
            </div>
          )}
        </div>
      )}
    </div>
  )
}

export function TopCommandBar({ onSearch }: { onSearch?: () => void }) {
  return (
    <header className="flex h-14 shrink-0 items-center gap-3 px-4">
      <button
        type="button"
        onClick={onSearch}
        className={cn(
          'flex h-8 flex-1 items-center gap-2 rounded-xl border border-line bg-panel/60 px-3',
          'text-2xs text-fg-dim transition-colors hover:border-line2 hover:text-fg-mid',
          onSearch === undefined && 'pointer-events-none opacity-60',
        )}
      >
        <Icon.Search className="size-3.5" strokeWidth={2} />
        Search markets
        <kbd className="ml-auto rounded border border-line2 px-1.5 py-px text-3xs text-fg-dim">
          /
        </kbd>
      </button>
      <GatewayStatus />
      <Clock />
    </header>
  )
}
