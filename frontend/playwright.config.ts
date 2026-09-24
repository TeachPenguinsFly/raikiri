import { defineConfig } from '@playwright/test'
import { k_terminal } from './e2e/preflight'

export default defineConfig({
  testDir: './e2e',
  globalSetup: './e2e/preflight.ts',
  timeout: 90_000,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: k_terminal,
    viewport: { width: 1600, height: 900 },
    trace: 'retain-on-failure',
  },
})
