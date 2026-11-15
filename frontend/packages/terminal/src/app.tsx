import { StrictMode, useEffect, useRef, useState, type ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter, Link, Navigate, Outlet, Route, Routes, useLocation } from 'react-router'
import { cn, ExchangeIcon, venueLabel, type AppModule } from '@raikiri/ui'
import { Frame, Glyph, Problem, type NavItem } from '@raikiri/ui/v3'
import { api } from './api/client'
import { useQuery } from './lib/query'
import { Login } from './routes/Login'

const k_health_refresh_ms = 5_000

export interface AppOptions {
  modules: readonly AppModule[]
  /** the mark on the login screen; the top bar carries the penguin */
  brand?: { nav: ReactNode; login: ReactNode } | undefined
}

type Health = ReturnType<typeof useHealth>

function useHealth() {
  return useQuery(() => api.health(), [], k_health_refresh_ms)
}

const k_market: Record<string, string> = { perp: 'Perpetual', spot: 'Spot', option: 'Options', future: 'Futures' }

type Feed = { state: 'live' | 'warming' | 'down'; word: string }

/** one gateway's state in words: down, up but still subscribing, or live */
function feedOf(g: { alive: boolean; coverage_pct: number }): Feed {
  if (!g.alive) return { state: 'down', word: 'Down' }
  if (g.coverage_pct < 50) return { state: 'warming', word: 'Warming up' }
  return { state: 'live', word: 'Live' }
}

const k_tone: Record<Feed['state'], string> = { live: 'bg-up', warming: 'bg-amber', down: 'bg-down' }

/** how many feeds are live, and behind it every gateway with its state and how much of its market is priced */
function Feeds({ health }: { health: Health }) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!open) return
    const off = (e: MouseEvent) => box.current !== null && !box.current.contains(e.target as Node) && setOpen(false)
    window.addEventListener('mousedown', off)
    return () => window.removeEventListener('mousedown', off)
  }, [open])
  if (health.state === 'failed') return <span className="text-sm text-amber">Offline</span>
  const gateways = health.state === 'ready' ? health.data.gateways : []
  const feeds = gateways.map((g) => ({ g, f: feedOf(g.gateway) }))
  const live = feeds.filter((x) => x.f.state === 'live').length
  const worst = feeds.some((x) => x.f.state === 'down')
    ? 'down'
    : feeds.some((x) => x.f.state === 'warming')
      ? 'warming'
      : 'live'
  const sorted = [...feeds].sort(
    (a, b) => venueLabel(a.g.venue).localeCompare(venueLabel(b.g.venue)) || a.g.market.localeCompare(b.g.market),
  )
  return (
    <div ref={box} className="relative max-[700px]:hidden">
      <button
        type="button"
        aria-expanded={open}
        onClick={() => setOpen((o) => !o)}
        className="flex h-7 cursor-pointer items-center gap-2 rounded-md px-2.5 text-sm text-fg-mid transition-colors hover:bg-hover hover:text-fg"
      >
        <span className={cn('size-2 rounded-full', k_tone[worst])} />
        Feeds
        <span className="mono text-fg">
          {live}/{feeds.length}
        </span>
        <Glyph name="down" className={cn('size-3.5 text-fg-dim transition-transform', open && 'rotate-180')} />
      </button>
      {open && (
        <div className="v3-pop absolute top-9 right-0 z-50 w-[27rem] rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_20px_50px_-10px_rgb(0_0_0/0.9)]">
          <div className="grid grid-cols-[1fr_auto_auto] gap-x-4 px-3 pt-2 pb-1.5 text-xs text-fg-dim">
            <span>Venue</span>
            <span className="text-right">Priced</span>
            <span>State</span>
          </div>
          <div className="max-h-[min(30rem,70vh)] overflow-y-auto">
            {sorted.map(({ g, f }) => (
              <div
                key={g.account}
                className="grid h-9 grid-cols-[1fr_auto_auto] items-center gap-x-4 rounded-sm px-3 text-sm hover:bg-hover"
              >
                <span className="flex min-w-0 items-center gap-2">
                  <ExchangeIcon venue={g.venue} className="size-4" />
                  <span className="shrink-0 text-fg">{venueLabel(g.venue)}</span>
                  <span className="text-fg-dim">{k_market[g.market] ?? g.market}</span>
                </span>
                <span className="mono text-right text-fg-mid">
                  {g.gateway.priced.toLocaleString('en-US')} / {g.gateway.instruments.toLocaleString('en-US')}
                </span>
                <span className="flex w-24 items-center gap-1.5 text-fg-mid">
                  <span className={cn('size-1.5 rounded-full', k_tone[f.state])} />
                  {f.word}
                </span>
              </div>
            ))}
          </div>
          <Link
            to="/settings"
            onClick={() => setOpen(false)}
            className="mt-1 flex h-8 items-center justify-center rounded-sm border-t border-line text-sm text-fg-mid hover:bg-hover hover:text-fg"
          >
            Open settings
          </Link>
        </div>
      )}
    </div>
  )
}

/** the top bar over every screen, fed by /v1/health: the feeds, a page when the backend is down, screens gated on
 *  their upstream */
export function Shell({ modules }: { modules: readonly AppModule[]; brand?: ReactNode | undefined }) {
  const health = useHealth()
  const { pathname } = useLocation()
  // behind the dev proxy a dead admind arrives as http 500 rather than a transport error
  const unreachable = health.state === 'failed' && (health.status === null || health.status >= 500)
  const upstreams = health.state === 'ready' ? (health.data.upstreams ?? []) : []
  const shown = modules.filter(
    (m) => m.requires === undefined || upstreams.some((u) => u.name === m.requires && u.answering),
  )
  const hidden =
    health.state !== 'loading' &&
    modules.some((m) => !shown.includes(m) && (pathname === m.path || pathname.startsWith(`${m.path}/`)))
  const nav: NavItem[] = shown.map((m) => ({
    to: m.path,
    label: m.label,
    ...(m.path === '/' && { match: /^\/$/ }),
  }))
  return (
    <Frame nav={nav} right={<Feeds health={health} />}>
      {unreachable ? (
        <Problem
          page
          glyph="plug"
          tone="amber"
          title="The terminal's server is not answering"
          detail={health.state === 'failed' ? health.detail : undefined}
        />
      ) : hidden ? (
        <Navigate to="/" replace />
      ) : (
        <div className="h-full min-h-0 overflow-hidden">
          <Outlet />
        </div>
      )}
    </Frame>
  )
}

export function App({ modules, brand }: AppOptions) {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="login" element={<Login brand={brand?.login} />} />
        <Route element={<Shell modules={modules} />}>
          {modules.flatMap((m) => [
            <Route key={m.path} path={m.path} element={m.element} />,
            ...(m.routes ?? []).map((r) => <Route key={r.path} path={r.path} element={r.element} />),
          ])}
          {/* a screen this app does not carry, e.g. a bookmark from the desk */}
          <Route path="*" element={<Navigate to="/" replace />} />
        </Route>
      </Routes>
    </BrowserRouter>
  )
}

export function mountApp(options: AppOptions, root = document.getElementById('root')) {
  if (root === null) throw new Error('#root is missing from index.html')
  createRoot(root).render(
    <StrictMode>
      <App {...options} />
    </StrictMode>,
  )
}
