import type { AppModule } from '@raikiri/ui'
import { MarketsPage } from './pages/MarketsPage'
import { SettingsPage } from './pages/SettingsPage'

export const marketsModule: AppModule = { path: '/', label: 'Markets', element: <MarketsPage /> }
export const settingsModule: AppModule = {
  path: '/settings',
  label: 'Settings',
  element: <SettingsPage />,
}

/** the public terminal's screens in nav order */
export const terminalModules: readonly AppModule[] = [marketsModule, settingsModule]
