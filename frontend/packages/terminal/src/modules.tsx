import type { AppModule } from '@raikiri/ui'
import { ExecutionPage } from './pages/ExecutionPage'
import { MarketsPage } from './pages/MarketsPage'
import { SettingsPage } from './pages/SettingsPage'
import { StrategiesPage } from './pages/StrategiesPage'
import { StrategyPage } from './pages/StrategyPage'
import { TreasuryPage } from './pages/TreasuryPage'

export const marketsModule: AppModule = { path: '/', label: 'Markets', element: <MarketsPage /> }
export const executionModule: AppModule = {
  path: '/execution',
  label: 'Execution',
  element: <ExecutionPage />,
}
export const strategiesModule: AppModule = {
  path: '/strategies',
  label: 'Strategies',
  element: <StrategiesPage />,
  requires: 'strategyd',
  routes: [{ path: '/strategies/:name', element: <StrategyPage /> }],
}
export const treasuryModule: AppModule = {
  path: '/treasury',
  label: 'Treasury',
  element: <TreasuryPage />,
}
export const settingsModule: AppModule = {
  path: '/settings',
  label: 'Settings',
  element: <SettingsPage />,
}

/** the public terminal's screens in nav order */
export const terminalModules: readonly AppModule[] = [
  marketsModule,
  executionModule,
  strategiesModule,
  treasuryModule,
  settingsModule,
]
