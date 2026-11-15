import { lazy, Suspense, type ReactNode } from 'react'
import type { AppModule } from '@raikiri/ui'
import { DashboardPage } from './crypto/DashboardPage'
import { OptionsPage } from './pages/OptionsPage'
import { SettingsPage } from './pages/SettingsPage'

// the terminal and its chart library load only when it opens
const TerminalPage = lazy(() => import('./crypto/terminal/TerminalPage').then((m) => ({ default: m.TerminalPage })))

export const dashboardModule: AppModule = { path: '/', label: 'Dashboard', element: <DashboardPage /> }
export const terminalModule: AppModule = {
  path: '/terminal',
  label: 'Terminal',
  element: (
    <Suspense fallback={<div className="h-full" />}>
      <TerminalPage />
    </Suspense>
  ),
}
/** a page laid out like prophet's: a gutter round it and its own scroll */
function Pane({ children }: { children: ReactNode }) {
  return <div className="h-full overflow-hidden p-2">{children}</div>
}

export const optionsModule: AppModule = {
  path: '/options',
  label: 'Options',
  element: (
    <Pane>
      <OptionsPage />
    </Pane>
  ),
}
export const settingsModule: AppModule = {
  path: '/settings',
  label: 'Settings',
  element: (
    <Pane>
      <SettingsPage />
    </Pane>
  ),
}

/** the public terminal's screens in nav order */
export const terminalModules: readonly AppModule[] = [dashboardModule, terminalModule, optionsModule, settingsModule]
