import { Outlet } from 'react-router'
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

  return (
    <div className="flex h-screen w-screen overflow-hidden bg-canvas">
      <SidebarNav />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopCommandBar health={health} />
        <main className="min-h-0 flex-1 overflow-hidden px-2 pb-2">
          {unreachable ? <ServiceDown detail={health.detail} /> : <Outlet />}
        </main>
      </div>
    </div>
  )
}
