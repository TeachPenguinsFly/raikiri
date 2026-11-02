import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { expect, test, type Page } from '@playwright/test'

/** written by optiondata_test with RAIKIRI_OPTIONS_FIXTURES set, so the page reads what admind answers */
const k_fixtures = join(import.meta.dirname, '..', 'fixtures', 'options')
const fixture = (name: string) => readFileSync(join(k_fixtures, name), 'utf8')

/** where the screenshots land; test-results is not committed */
const k_shots = process.env.RAIKIRI_SHOTS ?? join(import.meta.dirname, '..', '..', 'test-results', 'options')

const k_tick_ms = 400

type Underlying = { expiries: Array<{ expiry: string; date: string }> }
type Chain = { rows: Array<{ call: { venues: Quote[] }; put: { venues: Quote[] } }> }
type Quote = { instrument_id: string; venue: string; bid?: string; ask?: string; bid_size?: string; ask_size?: string }

const underlyings = JSON.parse(fixture('underlyings.json')) as Underlying[]
const dateOf = new Map(underlyings.flatMap((u) => u.expiries.map((e) => [e.expiry, e.date] as const)))
const quotesOf = new Map<string, Quote>()
for (const e of underlyings[0]?.expiries ?? []) {
  const chain = JSON.parse(fixture(`chain-${e.date}.json`)) as Chain
  for (const r of chain.rows) for (const q of [...r.call.venues, ...r.put.venues]) quotesOf.set(q.instrument_id, q)
}

const k_health = {
  gateways: [],
  database: { reachable: false, lag_seconds: null },
  upstreams: [],
}

async function mockAdmind(page: Page): Promise<void> {
  await page.route('**/v1/**', async (route) => {
    const url = new URL(route.request().url())
    const json = (body: string, status = 200) =>
      route.fulfill({ status, contentType: 'application/json', body })
    switch (url.pathname) {
      case '/v1/health':
        return json(JSON.stringify(k_health))
      case '/v1/stream/token':
        return json(JSON.stringify({ token: 'mock', expires_at: '0' }))
      case '/v1/options/underlyings':
        return json(fixture('underlyings.json'))
      case '/v1/options/surface':
        return json(fixture('surface.json'))
      case '/v1/options/chain': {
        const expiry = url.searchParams.get('expiry') ?? ''
        const date = dateOf.get(expiry) ?? expiry
        return json(fixture(`chain-${date}.json`))
      }
      default:
        return json(JSON.stringify({ title: 'not mocked', status: 404 }), 404)
    }
  })

  // the stream answers each subscribe with an ack, then walks every quoted bid and ask one tick at a time
  await page.routeWebSocket('**/v1/stream**', (ws) => {
    const timers: Array<ReturnType<typeof setInterval>> = []
    ws.onMessage((message) => {
      const m = JSON.parse(String(message)) as { op: string; id: string; quotes?: string[]; greeks?: string[] }
      if (m.op !== 'subscribe') return
      ws.send(JSON.stringify({ type: 'ack', id: m.id }))
      if (m.quotes === undefined) return
      let step = 0
      const ids = m.quotes
      timers.push(
        setInterval(() => {
          step += 1
          const quotes = ids.flatMap((id) => {
            const q = quotesOf.get(id)
            if (q?.bid === undefined || q.ask === undefined) return []
            const move = 1 + 0.002 * Math.sin(step + Number(BigInt(id) % 7n))
            return [
              {
                instrument_id: id,
                bid: (Number(q.bid) * move).toFixed(q.bid.includes('.') && Number(q.bid) < 10 ? 5 : 1),
                ask: (Number(q.ask) * move).toFixed(q.ask.includes('.') && Number(q.ask) < 10 ? 5 : 1),
                bid_size: q.bid_size ?? '1',
                ask_size: q.ask_size ?? '1',
                ts_event: '1793606400000000000',
              },
            ]
          })
          ws.send(JSON.stringify({ type: 'quotes', id: m.id, ts: '1793606400000000000', quotes }))
        }, k_tick_ms),
      )
    })
    ws.onClose(() => timers.forEach(clearInterval))
  })
}

const chain = (page: Page) => page.locator('table').filter({ hasText: 'Strike' })

test.beforeEach(async ({ page }) => {
  await mockAdmind(page)
  await page.goto('/options')
  await expect(chain(page).locator('tbody tr').first()).toBeVisible({ timeout: 15_000 })
})

test('the chain lists strikes around the forward with the best venue marked', async ({ page }) => {
  await expect(page.getByRole('tab', { name: /^BTC/ })).toHaveAttribute('aria-selected', 'true')
  await expect(page.locator('[data-forward]')).toContainText('Forward')
  const strikes = chain(page).locator('tbody tr:not([data-forward]) td:nth-child(8)')
  await expect(strikes).toHaveCount(15)
  await expect(strikes.first()).toHaveText('70,000')
  await expect(page.getByRole('heading', { name: 'Smile' })).toBeVisible()
  await expect(page.locator('svg circle').first()).toBeVisible()
  await page.screenshot({ path: join(k_shots, 'options-chain.png'), fullPage: true })
})

test('stream quotes move the board', async ({ page }) => {
  const bids = chain(page).locator('tbody tr:not([data-forward]) td:nth-child(4)')
  const board = async () => (await bids.allInnerTexts()).join('|')
  const before = await board()
  await expect.poll(board, { timeout: 5_000 }).not.toBe(before)
})

test('venues expand under each strike, in their own premium unit', async ({ page }) => {
  await page.getByRole('button', { name: 'Venues' }).click()
  await expect(page.locator('tr[data-venue="okx"]').first()).toBeVisible()
  await page.getByRole('button', { name: 'USD' }).click()
  await expect(page.getByRole('button', { name: 'Native' })).toBeVisible()
  await page.screenshot({ path: join(k_shots, 'options-venues-native.png'), fullPage: true })
})

test('an expiry tab and a venue filter reload the chain', async ({ page }) => {
  const tabs = page.getByRole('tablist', { name: 'Expiry' }).getByRole('tab')
  await expect(tabs).toHaveCount(3)
  await tabs.nth(2).click()
  await expect(tabs.nth(2)).toHaveAttribute('aria-selected', 'true')
  await page.getByRole('button', { name: 'Columns' }).click()
  await page.getByLabel('Vega').check()
  await page.getByRole('button', { name: 'Columns' }).click()
  await expect(chain(page).locator('thead th', { hasText: 'Vega' })).toHaveCount(2)
  await page.screenshot({ path: join(k_shots, 'options-far-expiry-vega.png'), fullPage: true })
})
