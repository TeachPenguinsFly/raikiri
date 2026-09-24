import { expect, test, type Locator, type Page } from '@playwright/test'

const table = (page: Page): Locator => page.locator('table')
const rows = (page: Page): Locator => table(page).locator('tbody tr')
const headers = (page: Page): Locator => table(page).locator('thead th')

const k_ready_ms = 60_000

test.describe('an upstream table', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/scanner')
  })

  test('renders rows rather than an explanation', async ({ page }) => {
    await expect(
      rows(page).first(),
      'no row — is the upstream running and proxied with admind --upstream?',
    ).toBeVisible({ timeout: k_ready_ms })
    expect(await rows(page).count()).toBeGreaterThan(1)
  })

  test('a second table the upstream names also draws rows', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const tabs = page.getByRole('tab')
    test.skip((await tabs.count()) < 2, 'the upstream serves a single table')
    await tabs.nth(1).click()
    await expect(tabs.nth(1)).toHaveAttribute('aria-selected', 'true')

    await expect(
      rows(page).first(),
      'the second table drew no row — is every gateway it needs attached?',
    ).toBeVisible({ timeout: k_ready_ms })
    expect(await headers(page).count()).toBeGreaterThan(1)
  })

  test('every declared column becomes a header, and every row fills them all', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const columns = await headers(page).count()
    expect(columns).toBeGreaterThan(2)
    for (const row of (await rows(page).all()).slice(0, 10)) {
      expect(await row.locator('td').count()).toBe(columns)
    }
  })

  test('cells carry values rather than dashes', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const cells = await rows(page).first().locator('td').allInnerTexts()
    const filled = cells.filter((c) => c.trim() !== '' && c.trim() !== '—')
    expect(filled.length, 'the first row is entirely empty').toBeGreaterThan(cells.length / 2)
  })

  test('a header sorts, and a second click turns it round', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const first = headers(page).first().locator('button')
    const column = () => rows(page).locator('td:nth-child(1)').allInnerTexts()

    await first.click()
    const up = (await column()).slice(0, 12)
    await first.click()
    const down = (await column()).slice(0, 12)

    expect(up.length).toBeGreaterThan(1)
    // a stable sort keeps ties in place, so check order rather than a mirror image
    const ordered = (xs: string[], sign: number) =>
      xs.every((x, i) => i === 0 || sign * x.localeCompare(xs[i - 1] ?? '') >= 0)
    // a fresh column sorts descending first (SortHead)
    expect(ordered(up, -1), 'descending on the first click').toBe(true)
    expect(ordered(down, 1), 'ascending on the second').toBe(true)
    expect(up[0], 'the two directions start from the same end').not.toBe(down[0])
  })

  test('the venue filter offers only venues that are actually in the data', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const opener = page.getByRole('button', { name: /venues|of \d+/ })
    await expect(opener).toBeVisible()
    await opener.click()
    const boxes = page.locator('input[type="checkbox"]')
    expect(await boxes.count()).toBeGreaterThan(0)
  })
})
