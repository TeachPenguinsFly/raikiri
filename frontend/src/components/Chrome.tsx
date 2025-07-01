import type { ReactNode } from 'react'
import { NavLink, Outlet } from 'react-router'
import { Wordmark } from '@/components/Brand'

const routes = [
  { to: '/', label: 'Overview', end: true },
  { to: '/treasury', label: 'Treasury' },
  { to: '/instruments', label: 'Instruments' },
  { to: '/options', label: 'Options' },
] as const

export function Chrome() {
  return (
    <div className="flex h-full flex-col">
      <header className="flex items-center gap-6 border-b border-line px-5 py-3">
        <Wordmark className="text-sm" />
        <nav className="flex gap-1">
          {routes.map((r) => (
            <NavLink
              key={r.to}
              to={r.to}
              end={'end' in r ? r.end : false}
              className={({ isActive }) =>
                `rounded px-3 py-1 text-sm ${
                  isActive ? 'bg-raised text-ink' : 'text-muted hover:text-ink'
                }`
              }
            >
              {r.label}
            </NavLink>
          ))}
        </nav>
      </header>
      <main className="min-h-0 flex-1 overflow-hidden">
        <Outlet />
      </main>
    </div>
  )
}

/** What a screen shows when the server refused or is unreachable. It names the
 *  status and the server's own words: "something went wrong" is not a thing a
 *  terminal may say about money. */
export function Failed({ title, detail, status }: { title: string; detail?: string | undefined; status: number | null }) {
  return (
    <div className="flex h-full items-center justify-center p-8">
      <div className="max-w-lg space-y-2 rounded border border-line bg-surface p-5">
        <div className="flex items-center gap-2">
          <span className="rounded bg-down/15 px-2 py-0.5 font-mono text-xs text-down">
            {status ?? 'offline'}
          </span>
          <span className="text-ink">{title}</span>
        </div>
        {detail !== undefined && <p className="text-sm text-muted">{detail}</p>}
        <p className="text-sm text-faint">
          admind serves this. Start it with <code className="text-muted">admind</code> and make
          sure a gateway is running.
        </p>
      </div>
    </div>
  )
}

export function Loading({ what }: { what: string }) {
  return <div className="p-8 text-sm text-muted">Reading {what}…</div>
}

export function Panel({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="rounded border border-line bg-surface">
      <h2 className="border-b border-line px-4 py-2 text-xs uppercase tracking-wider text-muted">
        {title}
      </h2>
      <div className="p-4">{children}</div>
    </section>
  )
}
