import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { expect, test, type Page } from '@playwright/test'

const k_fixtures = join(import.meta.dirname, '..', 'fixtures', 'blotter')
const fixture = (name: string) => readFileSync(join(k_fixtures, name), 'utf8')

const k_shots = process.env.RAIKIRI_SHOTS ?? join(import.meta.dirname, '..', '..', 'test-results', 'blotter')

type Row = { book_id?: number }

const k_fills = {
  items: [
    {
      venue: 'binance', exec_id: 'E-1001', client_order_id: '290341785029345280', instrument_id: '72057594037927936',
      side: 'sell', price: '68410', qty: '0.1', fee: '2.7', liquidity: 'taker', ts_event: '1793606399500000000',
      book_id: 1031, book_kind: 'hedge', book: 'hedge', parent_id: '7345112064000001', external_ref: 'rfq-12',
    },
    {
      venue: 'okx', exec_id: 'E-1002', client_order_id: '290480000000000999', instrument_id: '72057594037927937',
      side: 'buy', price: '3510.2', qty: '0.6', fee: '0.8', liquidity: 'maker', ts_event: '1793606381000000000',
      book_id: 1032, book_kind: 'client', book: 'client:acme', external_ref: 'acme-77431',
    },
  ],
}

const k_positions = [
  { instrument_id: '72057594037927936', strategy_id: 1031, book_kind: 'hedge', book: 'hedge', qty: '-0.18', realized: '0', ts_updated: '1793606399500000000' },
  { instrument_id: '72057594037927936', strategy_id: 1030, book_kind: 'manual', book: 'manual:vova', qty: '0.04', realized: '12.5', ts_updated: '1793606000000000000' },
  { instrument_id: '72057594037927937', strategy_id: 1032, book_kind: 'client', book: 'client:acme', qty: '0.6', realized: '0', ts_updated: '1793606381000000000' },
  { instrument_id: '72057594037927937', strategy_id: 7, book_kind: 'strategy', qty: '-0.3', realized: '41.2', ts_updated: '1793600000000000000' },
]

async function mockAdmind(page: Page): Promise<string[]> {
  const asked: string[] = []
  await page.route('**/v1/**', async (route) => {
    const url = new URL(route.request().url())
    const json = (body: string, status = 200) => route.fulfill({ status, contentType: 'application/json', body })
    const book = url.searchParams.get('book')
    const inBook = <T extends Row>(rows: T[]) => (book === null ? rows : rows.filter((r) => String(r.book_id) === book))
    switch (url.pathname) {
      case '/v1/health':
        return json(JSON.stringify({ gateways: [], database: { reachable: true, lag_seconds: 0 }, upstreams: [] }))
      case '/v1/stream/token':
        return json(JSON.stringify({ token: 'mock', expires_at: '0' }))
      case '/v1/markets':
        return json(fixture('markets.json'))
      case '/v1/books':
        return json(fixture('books.json'))
      case '/v1/joe/parents':
        return json(fixture('parents.json'))
      case '/v1/orders': {
        asked.push(url.search)
        if (url.searchParams.get('parent') !== null) return json(fixture('children.json'))
        const all = JSON.parse(fixture('orders.json')) as { items: Row[] }
        return json(JSON.stringify({ items: inBook(all.items) }))
      }
      case '/v1/fills':
        asked.push(url.search)
        return json(JSON.stringify({ items: inBook(k_fills.items) }))
      case '/v1/positions':
        asked.push(url.search)
        return json(
          JSON.stringify(
            k_positions.filter((p) => book === null || String(p.strategy_id) === book),
          ),
        )
      default:
        return json(JSON.stringify({ title: 'not mocked', status: 404 }), 404)
    }
  })
  await page.routeWebSocket('**/v1/stream**', (ws) => {
    ws.onMessage((message) => {
      const m = JSON.parse(String(message)) as { op: string; id: string }
      if (m.op === 'subscribe') ws.send(JSON.stringify({ type: 'ack', id: m.id }))
    })
  })
  return asked
}

const ordersTab = (page: Page) => page.getByRole('button', { name: /^orders/i })

test('orders say whose they are and why, and a parent opens into its children', async ({ page }) => {
  await mockAdmind(page)
  await page.goto('/')
  await ordersTab(page).click()

  const parent = page.locator('tr[data-parent="7345112064000001"]')
  await expect(parent).toContainText('hedge')
  await expect(parent).toContainText('rfq-12')
  await expect(parent).toContainText('twap')

  // a parent with no open child still has its row, because joe holds it
  const between = page.locator('tr[data-parent="7345112064000002"]')
  await expect(between).toContainText('manual:vova')
  // a finished parent has nothing left to show here
  await expect(page.locator('tr[data-parent="7345112064000000"]')).toHaveCount(0)

  await expect(page.getByRole('row', { name: /290200000000000123/ })).toContainText('by hand')
  await expect(page.getByRole('row', { name: /290480000000000999/ })).toContainText('client:acme')
  await expect(page.getByRole('row', { name: /290480000000000999/ })).toContainText('acme-77431')
  // nothing on record: the number is what there is, and it is still shown
  await expect(page.getByRole('row', { name: /1970324836974593/ })).toContainText('#7')

  const children = page.locator('tr[data-child="7345112064000001"]')
  await expect(children).toHaveCount(0)
  await parent.click()
  await expect(children).toHaveCount(3)
  await expect(children.first()).toContainText('rfq-12')
  await expect(children.nth(1)).toContainText('Filled')
  await page.screenshot({ path: join(k_shots, 'blotter-orders-parent-open.png') })
  await parent.click()
  await expect(children).toHaveCount(0)
})

test('the book filter narrows every tab', async ({ page }) => {
  const asked = await mockAdmind(page)
  await page.goto('/')
  await ordersTab(page).click()
  await expect(page.locator('tr[data-parent]')).toHaveCount(2)

  await page.getByLabel('Book').selectOption({ label: 'hedge' })
  await expect(page.locator('tr[data-parent]')).toHaveCount(1)
  await expect(page.getByRole('row', { name: /290200000000000123/ })).toHaveCount(0)
  await expect.poll(() => asked.some((q) => q.includes('book=1031'))).toBe(true)
  await page.screenshot({ path: join(k_shots, 'blotter-orders-hedge-book.png') })

  await page.getByRole('button', { name: /^trades/i }).click()
  await expect(page.getByRole('row', { name: /E-1001/ })).toContainText('hedge')
  await expect(page.getByRole('row', { name: /E-1002/ })).toHaveCount(0)

  await page.getByRole('button', { name: /^positions/i }).click()
  const rows = page.locator('[data-blotter] tbody tr').filter({ hasText: 'BTCUSDT' })
  await expect(rows).toHaveCount(1)
  await expect(rows.first()).toContainText('hedge')

  await page.getByLabel('Book').selectOption({ label: 'All books' })
  await expect(page.locator('[data-blotter] tbody tr').filter({ hasText: 'manual:vova' })).toHaveCount(1)
  await page.screenshot({ path: join(k_shots, 'blotter-positions.png') })
})
