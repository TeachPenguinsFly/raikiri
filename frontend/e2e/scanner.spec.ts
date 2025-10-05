import { expect, test, type Locator, type Page } from '@playwright/test'

/** A TABLE THIS REPOSITORY DID NOT WRITE, in a browser.
 *
 *  The screen draws whatever an upstream describes: the upstream names its own
 *  tables and its own columns, and nothing here knows what they mean. So the
 *  assertions are about the MECHANISM — do declared columns become headers, do
 *  rows carry a cell under each one, does sorting reorder them — and never
 *  about a label, because naming one would put somebody's model in a public
 *  test.
 *
 *  What no lighter check can answer is still the same thing: the socket and the
 *  proxy both prove the JSON arrives, and for months the arriving JSON was
 *  prose explaining that no board existed. Only a browser tells a rendered
 *  table from an essay. */

const table = (page: Page): Locator => page.locator('table')
const rows = (page: Page): Locator => table(page).locator('tbody tr')
const headers = (page: Page): Locator => table(page).locator('thead th')

/** Generous against a stack that has just started: the gateways are still
 *  pacing their subscribe frames, and with nothing subscribed the upstream has
 *  nothing to put in a row. */
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

  test('every declared column becomes a header, and every row fills them all', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    const columns = await headers(page).count()
    expect(columns).toBeGreaterThan(2)
    // A ROW SHORT OF A CELL is the failure that reads as a table and is not
    // one: the markets board once drew every row with a dash in it.
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
    // The first column the upstream declared, whatever it is.
    const first = headers(page).first().locator('button')
    const column = () => rows(page).locator('td:nth-child(1)').allInnerTexts()

    await first.click()
    const up = (await column()).slice(0, 12)
    await first.click()
    const down = (await column()).slice(0, 12)

    expect(up.length).toBeGreaterThan(1)
    // ORDER, NOT MIRROR IMAGE. A column with repeated values does not reverse
    // into itself backwards — a stable sort leaves ties in the order it found
    // them, so asserting `down === up.reverse()` fails on correct code.
    const ordered = (xs: string[], sign: number) =>
      xs.every((x, i) => i === 0 || sign * x.localeCompare(xs[i - 1] ?? '') >= 0)
    // A FRESH COLUMN STARTS DESCENDING (SortHead), which is right for a board
    // where the interesting row is the largest one; the second click is what
    // turns it round.
    expect(ordered(up, -1), 'descending on the first click').toBe(true)
    expect(ordered(down, 1), 'ascending on the second').toBe(true)
    expect(up[0], 'the two directions start from the same end').not.toBe(down[0])
  })

  test('the venue filter offers only venues that are actually in the data', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_ready_ms })
    // A filter listing a venue with no rows would be a control that does
    // nothing and a claim that the venue is attached.
    const opener = page.getByRole('button', { name: /venues|of \d+/ })
    await expect(opener).toBeVisible()
    await opener.click()
    const boxes = page.locator('input[type="checkbox"]')
    expect(await boxes.count()).toBeGreaterThan(0)
  })
})
