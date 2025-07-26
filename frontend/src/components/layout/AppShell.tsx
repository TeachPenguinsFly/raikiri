import { Outlet } from 'react-router'
import { SidebarNav } from './SidebarNav'
import { TopCommandBar } from './TopCommandBar'

export function AppShell() {
  return (
    <div className="aurora flex h-screen w-screen overflow-hidden">
      <SidebarNav />
      <div className="flex min-w-0 flex-1 flex-col">
        <TopCommandBar />
        <main className="min-h-0 flex-1 overflow-hidden">
          <Outlet />
        </main>
      </div>
    </div>
  )
}
