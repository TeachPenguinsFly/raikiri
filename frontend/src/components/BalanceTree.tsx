import { api, type AccountBalances } from '@/api/client'
import { EmptyState } from '@/components/shared/EmptyState'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { clockTime, nanosToMillis, toNumber } from '@/lib/format'
import { useQuery } from '@/lib/query'

/** Only the denomination is summed across a venue's wallets. Adding USDT to
 *  BTC produces a figure nobody holds. */
const k_denomination = 'USDT'

/** A balance is live in the gateway segment and nothing persists it, so the
 *  tree re-asks rather than ages. */
const k_refresh_ms = 5_000

const num = (v: number) =>
  v.toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: v !== 0 && Math.abs(v) < 1 ? 6 : 2,
  })

/** Perp is the unsuffixed default, and these are the only suffixes
 *  `format_account` mints — the account name carries the market. */
const k_suffixes: Array<[string, string]> = [
  ['-spot', 'spot'],
  ['-fut', 'future'],
  ['-opt', 'option'],
]

function marketOf(account: string): string {
  for (const [suffix, market] of k_suffixes) {
    if (account.endsWith(suffix)) return market
  }
  return 'perp'
}

/** A venue that cannot report answers with an empty `assets` and no
 *  `reported_at`. That is not an empty account, and it has nothing to put in a
 *  row. */
const answered = (a: AccountBalances): a is AccountBalances & { reported_at: string } =>
  a.reported_at !== null && a.reported_at !== undefined

/** ABSENT IS NOT ZERO. A venue that published no figure gets a dash; a zero
 *  drawn in its place is a holding the reader would go looking for. */
function figure(value: string | null | undefined, absent: string) {
  if (value === undefined || value === null) {
    return (
      <span className="text-fg-dim" title={absent}>
        —
      </span>
    )
  }
  return num(toNumber(value))
}

/** The tree, asking for its own balances. Rendered where nothing upstream has
 *  the rows already; the treasury passes its own and uses `BalanceRows`. */
export function BalanceTree() {
  const balances = useQuery(() => api.balances(), [], k_refresh_ms)

  if (balances.state === 'loading') return <EmptyState title="Asking the venues" />
  if (balances.state === 'failed') {
    return <EmptyState title={balances.title} hint={balances.detail} />
  }
  return <BalanceRows accounts={balances.data} />
}

/** Balances as a stepped list: venue, then the account it was held in, then
 *  the assets.
 *
 *  Flat, a desk running spot and perp on one exchange printed that exchange on
 *  every row — three `kucoin` lines that are one venue, two wallets and three
 *  holdings, and the reader has to do the grouping to see it. Stepped, the
 *  venue is said once and the shape of what is held under it is the shape of
 *  the list. */
export function BalanceRows({ accounts }: { accounts: AccountBalances[] }) {
  const reported = accounts.filter(answered)
  const venues = [...new Set(reported.map((a) => a.venue))]

  if (reported.length === 0) {
    return (
      <EmptyState
        title="No venue has answered with a balance"
        hint={`${accounts.length} accounts attached`}
      />
    )
  }

  return (
    <div className="pb-2">
      {/* The number columns are named once, at the top. A stepped list still
          has columns, and unlabelled ones make the reader guess which of the
          two is the free figure — and, in the third, whose clock it is. */}
      <div className="sticky top-0 z-10 flex h-6 items-center bg-panel pr-3 pl-9 shadow-[inset_0_-1px_0_0_var(--color-line)]">
        <span className="col-head">Asset</span>
        <span className="col-head ml-auto w-32 shrink-0 text-right">Total</span>
        <span className="col-head w-32 shrink-0 text-right">Available</span>
        <span className="col-head w-24 shrink-0 text-right">Reported</span>
      </div>
      {venues.map((venue) => {
        const rows = reported.filter((a) => a.venue === venue)
        const held = rows
          .flatMap((a) => a.assets)
          .filter((b) => b.asset === k_denomination && b.total !== undefined && b.total !== null)
        const usdt = held.length === 0 ? null : held.reduce((s, b) => s + toNumber(b.total), 0)
        return (
          <div key={venue}>
            <div className="flex h-8 items-center gap-3 px-3 shadow-[inset_0_-1px_0_0_var(--color-line)]">
              <Venue venue={venue} className="text-sm" />
              <span className="num ml-auto text-sm text-fg">
                {usdt === null ? (
                  <span
                    className="text-fg-dim"
                    title={`this venue published no ${k_denomination} total`}
                  >
                    —
                  </span>
                ) : (
                  num(usdt)
                )}
              </span>
              <span className="w-12 text-right text-2xs text-fg-dim">{k_denomination}</span>
            </div>
            {rows.map((a) => (
              <div key={a.account}>
                {/* One account is one wallet, so the market line is where the
                    venue's clock belongs: staleness is the account having gone
                    quiet, not any one asset. */}
                <div className="flex h-5 items-center pr-3 pl-9">
                  <span className="col-head">{marketOf(a.account)}</span>
                  <span
                    className="ml-auto w-24 shrink-0 text-right text-2xs"
                    title={
                      a.stale
                        ? `the venue last answered at ${clockTime(nanosToMillis(a.reported_at))}; these are still those figures and nothing has confirmed them since`
                        : undefined
                    }
                  >
                    {a.stale ? (
                      <StatusBadge status="stale" />
                    ) : (
                      <span className="num text-fg-dim">
                        {clockTime(nanosToMillis(a.reported_at))}
                      </span>
                    )}
                  </span>
                </div>
                {a.assets.map((b) => (
                  <div key={`${a.account}:${b.asset}`} className="row flex items-center pr-3 pl-9">
                    <span className="text-sm text-fg-mid">{b.asset}</span>
                    <span className="num ml-auto w-32 shrink-0 text-right text-sm text-fg">
                      {figure(b.total, 'this venue published no total for this asset')}
                    </span>
                    {/* A venue that publishes a total and no free figure
                        leaves a dash. Repeating the total there invites
                        committing margin twice. */}
                    <span className="num w-32 shrink-0 text-right text-sm text-fg-mid">
                      {figure(b.available, 'this venue publishes no per-asset free figure')}
                    </span>
                    <span className="w-24 shrink-0" />
                  </div>
                ))}
              </div>
            ))}
          </div>
        )
      })}
    </div>
  )
}
