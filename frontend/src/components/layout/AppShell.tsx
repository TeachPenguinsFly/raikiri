import { Navigate, Outlet, useLocation } from 'react-router'
import { api } from '@/api/client'
import { ServiceDown } from '@/components/shared/ServiceDown'
import { useQuery } from '@/lib/query'
import { SidebarNav } from './SidebarNav'
import { TopCommandBar } from './TopCommandBar'

const k_health_refresh_ms = 5_000

export function AppShell() {
  const health = useQuery(() => api.health(), [], k_health_refresh_ms)
  // behind the dev proxy a dead admind arrives as http 500 rather than a transport error
  const unreachable =
    health.state === 'failed' && (health.status === null || health.status >= 500)

  // a screen whose backing service admind does not report is left out of the nav
  const { pathname } = useLocation()
  const upstreams = health.state === 'ready' ? (health.data.upstreams ?? []) : []
  const hidden = new Set<string>()
  // the scanner's tables come from a service outside this repository
  if (!upstreams.some((u) => u.name === 'boards')) hidden.add('/scanner')
  if (!upstreams.some((u) => u.name === 'strategyd' && u.answering)) hidden.add('/strategies')
  const off =
    health.state !== 'loading' &&
    [...hidden].some((path) => pathname === path || pathname.startsWith(`${path}/`))

  return (
    <div className="flex h-screen w-screen overflow-hidden bg-canvas">
      <SidebarNav hidden={hidden} />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopCommandBar health={health} />
        <main className="min-h-0 flex-1 overflow-hidden px-2 pb-2">
          {unreachable ? (
            <ServiceDown detail={health.detail} />
          ) : off ? (
            <Navigate to="/" replace />
          ) : (
            <Outlet />
          )}
        </main>
      </div>
    </div>
  )
}
