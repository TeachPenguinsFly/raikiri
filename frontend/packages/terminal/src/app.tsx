import { StrictMode, type ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter, Link, Navigate, Outlet, Route, Routes, useLocation } from 'react-router'
import { cn, ExchangeIcon, venueLabel, type AppModule } from '@raikiri/ui'
import { Frame, Problem, type NavItem } from '@raikiri/ui/v3'
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

/** each venue's mark with a dot for whether its feed is up; the dashboard says more */
function Feeds({ health }: { health: Health }) {
  const gateways = health.state === 'ready' ? health.data.gateways : []
  return (
    <Link
      to="/"
      className="flex h-7 items-center gap-2.5 rounded-md px-2 text-2xs text-fg-dim transition-colors hover:bg-hover max-[700px]:hidden"
    >
      {health.state === 'failed' && <span className="text-amber">Offline</span>}
      {gateways.map((g) => {
        const live = g.gateway.alive && g.gateway.coverage_pct >= 50
        return (
          <span key={g.account} className="flex items-center gap-1" title={`${venueLabel(g.venue)} ${g.account}`}>
            <ExchangeIcon venue={g.venue} className="size-3.5" />
            <span className={cn('size-1.5 rounded-full', live ? 'bg-up' : g.gateway.alive ? 'bg-amber' : 'bg-down')} />
          </span>
        )
      })}
    </Link>
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
