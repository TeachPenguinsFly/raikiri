import { expect, test, type Locator, type Page } from '@playwright/test'

/** THE FUNDING BOARD, IN A BROWSER.
 *
 *  The socket tests and `tools/coverage_smoke.sh` prove the numbers reach the
 *  page's doorstep. Neither can tell a rendered board from the essay this
 *  screen used to be: both answers are a 200 with a body, and for months the
 *  body was prose explaining that no board existed. The only check that
 *  distinguishes them is one that reads a cell.
 *
 *  What is asserted is a NUMBER in the funding column, not a row. A row with a
 *  dash in it is exactly the failure the markets board shipped once — every row
 *  present, every cell empty. */

/** The board's own column, and the one no other table on this screen has. */
const board = (page: Page): Locator =>
  page.locator('table').filter({ hasText: 'Funding 8h, bp' })

const rows = (page: Page): Locator => board(page).locator('tbody tr')

/** Basis points as the page formats them: `toFixed(1)`, so digits and one
 *  point. An em-dash or an empty cell fails this, which is the point. */
const k_bp = /^-?\d+\.\d$/

/** An integer, possibly negative — the entry cost. Zero is a real answer here
 *  (two venues quoting the same price) and must not be confused with absent. */
const k_int = /^-?\d+$/

/** kowalski rebuilds once a second and admind polls it on request, so a board
 *  that is going to appear appears quickly. This is generous against a stack
 *  that has just started and whose gateways are still pacing their subscribe
 *  frames — with nothing subscribed there are no legs to pair. */
const k_board_ms = 60_000

test.describe('funding board', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/scanner')
  })

  test('shows priced rows rather than an explanation', async ({ page }) => {
    await expect(
      rows(page).first(),
      'no row on the funding board — is kowalski running, and are two venues quoting one symbol?',
    ).toBeVisible({ timeout: k_board_ms })
    expect(await rows(page).count()).toBeGreaterThan(1)
  })

  test('every funding edge is a number, and every one of them is positive', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_board_ms })
    // ORIENTED SO THE EDGE IS POSITIVE. A negative row would mean the board
    // named the legs the wrong way round, which is the same information written
    // backwards and makes a reader flip every row in their head.
    const edges = await rows(page).locator('td:nth-child(4)').allInnerTexts()
    expect(edges.length).toBeGreaterThan(0)
    for (const cell of edges.slice(0, 20)) {
      expect(cell.trim(), 'a funding cell that is not a number').toMatch(k_bp)
      expect(Number(cell), 'the board is oriented so the held leg is the cheaper one').toBeGreaterThanOrEqual(0)
    }
  })

  test('the entry cost is shown beside the edge and never folded into it', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_board_ms })
    const entries = await rows(page).locator('td:nth-child(5)').allInnerTexts()
    for (const cell of entries.slice(0, 20)) {
      expect(cell.trim(), 'an entry cell that is not a number').toMatch(k_int)
    }
    // The two columns are independent: if they had been combined into one
    // score, the fifth column would track the fourth. Finding any row whose
    // entry cost is negative while its funding edge is positive proves they
    // are still separate — and that pair is the one a combined score would
    // have floated to the top.
    const edges = await rows(page).locator('td:nth-child(4)').allInnerTexts()
    const bothWays = edges.some((e, i) => Number(e) > 0 && Number(entries[i] ?? '0') < 0)
    expect(bothWays, 'no row pays funding while costing something to enter — check the sample').toBe(
      true,
    )
  })

  test('the ranking is by funding, descending', async ({ page }) => {
    await expect(rows(page).first()).toBeVisible({ timeout: k_board_ms })
    const edges = (await rows(page).locator('td:nth-child(4)').allInnerTexts())
      .slice(0, 30)
      .map(Number)
    for (let i = 1; i < edges.length; i += 1) {
      expect(edges[i] ?? 0, `row ${i} outranks the one above it`).toBeLessThanOrEqual(
        edges[i - 1] ?? 0,
      )
    }
  })
})
