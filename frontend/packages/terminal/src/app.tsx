import { StrictMode, type ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import { BrowserRouter, Route, Routes } from 'react-router'
import { AppShell, ServiceDown, type AppModule } from '@raikiri/ui'
import { api } from './api/client'
import { GatewayStatus } from './components/layout/GatewayStatus'
import { useQuery } from './lib/query'
import { Login } from './routes/Login'

const k_health_refresh_ms = 5_000

export interface AppOptions {
  modules: readonly AppModule[]
  /** the mark in the sidebar and on the login screen */
  brand?: { nav: ReactNode; login: ReactNode } | undefined
}

/** the shell fed by /v1/health: gateway status, a 500 screen, modules gated on their upstream */
export function Shell({
  modules,
  brand,
}: {
  modules: readonly AppModule[]
  brand?: ReactNode | undefined
}) {
  const health = useQuery(() => api.health(), [], k_health_refresh_ms)
  // behind the dev proxy a dead admind arrives as http 500 rather than a transport error
  const unreachable =
    health.state === 'failed' && (health.status === null || health.status >= 500)

  const upstreams = health.state === 'ready' ? (health.data.upstreams ?? []) : []
  const hidden = new Set(
    modules
      .filter((m) => m.requires !== undefined)
      .filter((m) => !upstreams.some((u) => u.name === m.requires && u.answering))
      .map((m) => m.path),
  )

  return (
    <AppShell
      modules={modules}
      brand={brand}
      hidden={hidden}
      settled={health.state !== 'loading'}
      status={<GatewayStatus health={health} />}
      down={unreachable ? <ServiceDown detail={health.detail} /> : undefined}
    />
  )
}

export function App({ modules, brand }: AppOptions) {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="login" element={<Login brand={brand?.login} />} />
        <Route element={<Shell modules={modules} brand={brand?.nav} />}>
          {modules.flatMap((m) => [
            <Route key={m.path} path={m.path} element={m.element} />,
            ...(m.routes ?? []).map((r) => <Route key={r.path} path={r.path} element={r.element} />),
          ])}
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
