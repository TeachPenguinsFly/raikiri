import { useState, type ReactNode } from 'react'
import { Navigate, Outlet, useLocation } from 'react-router'
import { BrandMark } from '../shared/BrandMark'
import { SidebarNav } from './SidebarNav'
import { TopCommandBar } from './TopCommandBar'
import { TopBarTools } from './Workspace'

/** a screen of an app: its nav entry and its routes */
export interface AppModule {
  /** '/' or '/name' */
  path: string
  label: string
  icon?: ReactNode
  element: ReactNode
  /** an upstream that must be answering on /v1/health before the screen is listed */
  requires?: string
  /** further routes the module owns, absolute, e.g. '/strategies/:name' */
  routes?: Array<{ path: string; element: ReactNode }>
}

export function AppShell({
  modules,
  brand = <BrandMark className="text-xs" />,
  hidden = new Set(),
  settled = true,
  status,
  down,
}: {
  modules: readonly AppModule[]
  brand?: ReactNode | undefined
  /** module paths left out of the nav; a direct visit goes to / once settled */
  hidden?: ReadonlySet<string> | undefined
  settled?: boolean | undefined
  /** the right side of the top bar, before the clock */
  status?: ReactNode | undefined
  /** shown instead of every screen, e.g. when the backend is unreachable */
  down?: ReactNode | undefined
}) {
  const { pathname } = useLocation()
  const [tools, setTools] = useState<HTMLElement | null>(null)
  const off =
    settled && [...hidden].some((path) => pathname === path || pathname.startsWith(`${path}/`))

  return (
    <div className="flex h-screen w-screen overflow-hidden bg-canvas">
      <SidebarNav brand={brand} modules={modules.filter(({ path }) => !hidden.has(path))} />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopCommandBar tools={setTools}>{status}</TopCommandBar>
        <main className="min-h-0 flex-1 overflow-hidden px-2 pb-2">
          <TopBarTools.Provider value={tools}>
            {down !== undefined && down !== null ? down : off ? <Navigate to="/" replace /> : <Outlet />}
          </TopBarTools.Provider>
        </main>
      </div>
    </div>
  )
}
