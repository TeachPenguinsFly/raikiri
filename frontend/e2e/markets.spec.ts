import { expect, test, type Locator, type Page } from '@playwright/test'

/** THE TERMINAL IN A BROWSER, because nothing under it answers the question.
 *
 *  `tools/terminal_smoke.sh` proves a quote reaches the browser's doorstep, and
 *  says itself that it stops there. On 2026-09-06 the doorstep was not the
 *  problem: admind delivered quotes correctly and the board showed a column of
 *  em-dashes for a whole session, because `useQuotes` merged its rows into a
 *  Map held in a `useRef` and handed the same object back forever. The rows are
 *  built in a `useMemo([selected, quotes, funding, ticker])`, and an identity
 *  that never changes is a dependency that never changes, so the memo never
 *  recomputed. The socket was perfect, the smoke test was green, and the screen
 *  was empty. Only a browser fails on that.
 *
 *  Which is why the assertion this file exists for is on the TEXT OF A CELL and
 *  not on a row existing: every row was there that day, each with a dash in it.
 */

/** A price as the board formats it — `toFixed`, so digits and one point, no
 *  separators. An em-dash is what a venue that has published nothing prints,
 *  and telling the two apart is the whole job. */
const k_price = /^\d+\.\d+$/

/** Liquid on every venue a desk attaches, and quoting continuously on all of
 *  them. A dash here is the terminal's fault; it is never the market's. */
const k_pair = 'BTC/USDT'

/** The catalogue, which is one HTTP call and may be answered by an admind that
 *  has just started. Nothing about the row memo rides on this deadline, so it is
 *  the one wait here that may be as long as it likes. */
const k_catalogue_ms = 30_000

/** How long a subscription's first frame is waited for.
 *
 *  UNDER THE CATALOGUE'S THIRTY-SECOND REFRESH, and that is a constraint rather
 *  than a taste. `useQuery` hands `MarketsPage` freshly built market objects on
 *  every poll, and that alone recomputes the row memo — so a board that redraws
 *  on nothing but the catalogue passes any deadline set above it, thirty-second
 *  old figures and all. Measured, not assumed: with the ref-mutating `useMerged`
 *  put back, thirty seconds passes here and fifteen does not. */
const k_first_quote_ms = 15_000

/** How long a bid may sit still before the feed behind it is dead rather than
 *  quiet. Perp BTC ticks several times a second on every venue that lists it,
 *  and this sits under the catalogue refresh for the same reason as above. */
const k_tick_ms = 15_000

/** The board, told apart from the blotter under it by the one column only the
 *  board has. Both are plain tables and neither is the page's landmark; the
 *  sort control in the header is the stable accessible thing about it. */
const board = (page: Page): Locator =>
  page.locator('table').filter({ has: page.getByRole('button', { name: 'Spread bp', exact: true }) })

const rows = (page: Page): Locator => board(page).locator('tbody tr')

/** The Bid column. Third by position, which is asserted rather than assumed —
 *  see the header check below — because a column inserted to its left would
 *  otherwise turn every assertion in this file into an assertion about Ask. */
const bids = (page: Page): Locator => rows(page).locator('td:nth-child(3)')

/** The sidebar names a pair across two spans and prints its venue count beside
 *  it, so the row is matched from the front: a bare `BTC/USDT` also selects
 *  WBTC/USDT. */
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
  // The board is one row per listing, so a first column with nothing in it is
  // a row the catalogue did not fill.
  expect(await rows(page).locator('td:first-child').allInnerTexts()).not.toContain('')

  await expect(board(page).locator('thead th').nth(2)).toHaveText(/^Bid/)
})

test('a venue quotes a number, not a dash', async ({ page }) => {
  await open(page, k_pair)

  // THE ONE THAT WOULD HAVE FAILED. Not `not.toHaveCount(0)` on the rows: the
  // rows were all present on the day this file is about.
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

  // THE WHOLE BOARD, not one cell. A single bid is a poor liveness signal:
  // measured against the live venues, BTC's best bid took only three to five
  // distinct values in twenty seconds, so one cell sitting still for the whole
  // window is an ordinary quiet minute and not a dead feed. Every cell sitting
  // still is the failure worth having — it is what the screen showed on
  // 2026-09-06, and it stays caught here.
  const board = async () => (await bids(page).allInnerTexts()).join('|')
  const before = await board()

  await expect
    .poll(board, {
      timeout: k_tick_ms,
      message: `nothing on the board moved in ${k_tick_ms / 1000}s — the feed behind it is dead`,
    })
    .not.toBe(before)
})
