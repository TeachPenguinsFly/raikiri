import { expect, test, type Locator, type Page } from '@playwright/test'

/** a price as the board formats it; a dash means the venue published nothing */
const k_price = /^\d+\.\d+$/

const k_pair = 'BTC/USDT'

const k_catalogue_ms = 30_000

/** kept under the catalogue's 30 s refresh, which also redraws the board */
const k_first_quote_ms = 15_000

const k_tick_ms = 15_000

const board = (page: Page): Locator =>
  page.locator('table').filter({ has: page.getByRole('button', { name: 'Spread bp', exact: true }) })

const rows = (page: Page): Locator => board(page).locator('tbody tr')

/** bid is the third column; the header check below asserts it */
const bids = (page: Page): Locator => rows(page).locator('td:nth-child(3)')

/** matched from the front: a bare BTC/USDT also matches WBTC/USDT */
async function open(page: Page, symbol: string): Promise<void> {
  await page.goto('/')
  const pair = page.getByRole('button', { name: new RegExp(`^${symbol}\\b`) })
  await expect(pair, `${symbol} is not in the catalogue — which gateway was meant to load it?`)
    .toBeVisible({ timeout: k_catalogue_ms })
  await pair.click()
}

test('the catalogue arrives and lists pairs', async ({ page }) => {
  await page.goto('/')
  const pairs = page.getByRole('button', { name: /^\S+\/\S+\s\d+$/ })
  await expect(pairs.first()).toBeVisible({ timeout: k_catalogue_ms })
  expect(await pairs.count()).toBeGreaterThan(0)
})

test('a pair opens onto its venues', async ({ page }) => {
  await open(page, k_pair)

  await expect(rows(page).first()).toBeVisible()
  expect(await rows(page).locator('td:first-child').allInnerTexts()).not.toContain('')

  await expect(board(page).locator('thead th').nth(2)).toHaveText(/^Bid/)
})

test('a venue quotes a number, not a dash', async ({ page }) => {
  await open(page, k_pair)

  await expect(
    bids(page).filter({ hasText: k_price }).first(),
    'every venue on the board is showing a dash — the quotes are arriving and the table is not redrawing',
  ).toHaveText(k_price, { timeout: k_first_quote_ms })
})

test('the numbers move', async ({ page }) => {
  await open(page, k_pair)
  await expect(bids(page).filter({ hasText: k_price }).first()).toHaveText(k_price, {
    timeout: k_first_quote_ms,
  })

  // assert on the whole board; one bid can sit still through a quiet minute
  const board = async () => (await bids(page).allInnerTexts()).join('|')
  const before = await board()

  await expect
    .poll(board, {
      timeout: k_tick_ms,
      message: `nothing on the board moved in ${k_tick_ms / 1000}s — the feed behind it is dead`,
    })
    .not.toBe(before)
})
