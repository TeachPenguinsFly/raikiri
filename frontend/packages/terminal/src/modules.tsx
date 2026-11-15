import { lazy, Suspense, type ReactNode } from 'react'
import type { AppModule } from '@raikiri/ui'
import { Navigate } from 'react-router'
import { OptionsPage } from './pages/OptionsPage'
import { SettingsPage } from './pages/SettingsPage'

// the terminal and its chart library load only when it opens
const TerminalPage = lazy(() => import('./crypto/terminal/TerminalPage').then((m) => ({ default: m.TerminalPage })))

export const terminalModule: AppModule = {
  path: '/',
  label: 'Terminal',
  // the address it had before it became the home screen
  routes: [{ path: '/terminal', element: <Navigate to="/" replace /> }],
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
export const terminalModules: readonly AppModule[] = [terminalModule, optionsModule, settingsModule]
