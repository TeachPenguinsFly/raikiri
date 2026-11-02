import { defineConfig } from '@playwright/test'

/** its own port, so a running `make up` stack on 5173 is left alone */
const k_port = 5174

/** the terminal against canned admind answers: no gateway, no venue, no socket off this machine */
export default defineConfig({
  testDir: './e2e/mock',
  timeout: 60_000,
  retries: 0,
  reporter: [['list']],
  use: {
    baseURL: `http://127.0.0.1:${k_port}`,
    viewport: { width: 1600, height: 900 },
    trace: 'retain-on-failure',
  },
  webServer: {
    command: `npm run dev -w raikiri-terminal -- --port ${k_port} --strictPort --host 127.0.0.1`,
    url: `http://127.0.0.1:${k_port}`,
    reuseExistingServer: false,
    timeout: 60_000,
  },
})
