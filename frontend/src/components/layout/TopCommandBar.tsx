import { useEffect, useRef, useState } from 'react'
import { api } from '@/api/client'
import { useQuery } from '@/lib/query'
import { cn } from '@/lib/utils'

/** utc, as venues and the journal stamp */
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

const k_health_refresh_ms = 5_000

function GatewayStatus() {
  const health = useQuery(() => api.health(), [], k_health_refresh_ms)
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

  if (health.state === 'loading') {
    return (
      <div className="flex h-6 items-center gap-2 px-2 text-xs text-fg-dim">
        <span className="size-[5px] rounded-full bg-fg-dim" />
        <span>gateways</span>
      </div>
    )
  }

  const failed = health.state === 'failed'
  const gateways = health.state === 'ready' ? health.data.gateways : []
  const alive = gateways.filter((g) => g.gateway.alive).length
  const all = gateways.length
  const tone = failed || alive === 0 ? 'bg-down' : alive === all ? 'bg-up' : 'bg-amber'

  return (
    <div className="relative" ref={ref}>
      <button
        type="button"
        onClick={() => setOpen((v) => !v)}
        className={cn(
          'flex h-6 items-center gap-2 rounded-sm px-2 text-xs transition-colors duration-150',
          open ? 'bg-active text-fg' : 'text-fg-mid hover:bg-hover hover:text-fg',
        )}
      >
        <span
          className={cn('size-[5px] rounded-full', tone, (failed || alive < all) && 'pulse-dot')}
        />
        {health.state === 'failed' ? (
          <span className="max-w-44 truncate">{health.title}</span>
        ) : (
          <>
            <span className="num tabular-nums">
              {alive}/{all}
            </span>
            <span className="text-fg-dim">gateways</span>
          </>
        )}
      </button>

      {open && (
        <div className="absolute right-0 top-7 z-30 w-56 rounded-md bg-raised p-1 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.8)]">
          {health.state === 'failed' ? (
            <div className="px-2 py-2 text-xs">
              <div className="text-down">
                {health.status !== null && (
                  <span className="num mr-2 text-fg-dim">{health.status}</span>
                )}
                {health.title}
              </div>
              {health.detail !== undefined && (
                <div className="mt-1 break-words text-fg-dim">{health.detail}</div>
              )}
            </div>
          ) : (
            <>
              {health.data.gateways.map((g) => (
                <div key={g.account} className="flex h-6 items-center gap-2 px-2 text-xs">
                  <span
                    className={cn(
                      'size-[5px] rounded-full',
                      g.gateway.alive ? 'bg-up' : 'bg-down',
                    )}
                  />
                  <span className="text-fg">{g.account}</span>
                  <span className="num ml-auto text-xs text-fg-dim">
                    {g.gateway.alive ? g.gateway.instruments.toLocaleString() : '—'}
                  </span>
                </div>
              ))}
              {health.data.gateways.length === 0 && (
                <div className="px-2 py-3 text-center text-xs text-fg-dim">
                  No gateway is attached.
                </div>
              )}
            </>
          )}
        </div>
      )}
    </div>
  )
}

export function TopCommandBar() {
  return (
    <header className="flex h-10 shrink-0 items-center justify-end gap-3 px-3">
      <GatewayStatus />
      <span className="h-4 w-px bg-line" />
      <Clock />
    </header>
  )
}
