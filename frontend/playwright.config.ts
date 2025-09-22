import { defineConfig } from '@playwright/test'
import { k_terminal } from './e2e/preflight'

export default defineConfig({
  testDir: './e2e',
  globalSetup: './e2e/preflight.ts',
  // The subscription's first frame is worth waiting on, and two of the four
  // tests wait for one. Nothing else here is slow.
  timeout: 90_000,
  // NO RETRIES. A board that needed a second run to show a price is the defect
  // this suite was written for, not a flake to be papered over.
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: k_terminal,
    // Three panels and an eight-column board. At the 1280 default the board is
    // behind a horizontal scroll, which is not what any of this measures.
    viewport: { width: 1600, height: 900 },
    trace: 'retain-on-failure',
  },
})
