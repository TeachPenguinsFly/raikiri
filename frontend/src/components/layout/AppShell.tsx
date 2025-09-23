import { Outlet } from 'react-router'
import { api } from '@/api/client'
import { ServiceDown } from '@/components/shared/ServiceDown'
import { useQuery } from '@/lib/query'
import { SidebarNav } from './SidebarNav'
import { TopCommandBar } from './TopCommandBar'

export function AppShell() {
  // HEALTH IS THE ONE CALL THAT ANSWERS "is anything there". Every screen
  // needs admind, so a screen that cannot reach it has nothing true to draw.
  //
  // BOTH SHAPES OF UNREACHABLE, because the two deployments fail differently:
  // served straight, a dead admind is a transport error and `status` is null;
  // behind the dev server's proxy the same outage arrives as HTTP 500, since
  // the proxy answers on admind's behalf. Checking only for the first showed
  // the page as normal with every panel empty, which is the exact lie this
  // component exists to stop.
  const health = useQuery(() => api.health(), [], 5_000)
  const unreachable =
    health.state === 'failed' && (health.status === null || health.status >= 500)

  return (
    <div className="flex h-screen w-screen overflow-hidden bg-canvas">
      <SidebarNav />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopCommandBar />
        <main className="min-h-0 flex-1 overflow-hidden px-2 pb-2">
          {unreachable ? <ServiceDown detail={health.detail} /> : <Outlet />}
        </main>
      </div>
    </div>
  )
}
