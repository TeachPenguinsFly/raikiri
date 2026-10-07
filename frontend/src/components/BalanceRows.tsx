import type { AccountBalances } from '@/api/client'
import { EmptyState } from '@/components/shared/EmptyState'
import { Silent } from '@/components/shared/Silent'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { Venue } from '@/components/shared/Venue'
import { clockTime, nanosToMillis, toNumber } from '@/lib/format'

/** only this denomination is summed across a venue's wallets */
const k_denomination = 'USDT'

const amount = (v: number) =>
  v.toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: v !== 0 && Math.abs(v) < 1 ? 6 : 2,
  })

/** suffixes minted by format_account; perp is unsuffixed */
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

/** a venue that cannot report sends empty assets and no reported_at */
const answered = (a: AccountBalances): a is AccountBalances & { reported_at: string } =>
  a.reported_at !== null && a.reported_at !== undefined

function figure(value: string | null | undefined, absent: string) {
  return value === undefined || value === null ? <Silent title={absent} /> : amount(toNumber(value))
}

export function BalanceRows({ accounts }: { accounts: AccountBalances[] }) {
  const reported = accounts.filter(answered)
  const venues = [...new Set(reported.map((a) => a.venue))]

  if (reported.length === 0) {
    return (
      <EmptyState
        title="No balances yet"
        hint={`a gateway reports its wallets once it runs with an API key; ${accounts.length} accounts attached on public data`}
      />
    )
  }

  return (
    <div className="pb-2">
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
                  <Silent title={`this venue published no ${k_denomination} total`} />
                ) : (
                  amount(usdt)
                )}
              </span>
              <span className="w-12 text-right text-2xs text-fg-dim">{k_denomination}</span>
            </div>
            {rows.map((a) => (
              <div key={a.account}>
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
                    {/* no free figure leaves a dash; repeating the total would double-count margin */}
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
