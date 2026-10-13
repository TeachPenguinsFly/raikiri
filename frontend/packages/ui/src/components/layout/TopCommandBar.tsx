import { useEffect, useState, type ReactNode } from 'react'

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

export function TopCommandBar({ children }: { children?: ReactNode }) {
  return (
    <header className="flex h-10 shrink-0 items-center justify-end gap-3 px-3">
      {children !== undefined && children !== null && (
        <>
          {children}
          <span className="h-4 w-px bg-line" />
        </>
      )}
      <Clock />
    </header>
  )
}
