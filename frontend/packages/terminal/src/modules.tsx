import type { AppModule } from '@raikiri/ui'
import { MarketsPage } from './pages/MarketsPage'
import { OptionsPage } from './pages/OptionsPage'
import { SettingsPage } from './pages/SettingsPage'

export const marketsModule: AppModule = { path: '/', label: 'Markets', element: <MarketsPage /> }
export const optionsModule: AppModule = { path: '/options', label: 'Options', element: <OptionsPage /> }
export const settingsModule: AppModule = {
  path: '/settings',
  label: 'Settings',
  element: <SettingsPage />,
}

/** the public terminal's screens in nav order */
export const terminalModules: readonly AppModule[] = [marketsModule, optionsModule, settingsModule]
