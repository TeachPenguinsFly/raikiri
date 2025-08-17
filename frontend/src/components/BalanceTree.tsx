import { Venue } from '@/components/shared/Venue'
import { BALANCES } from '@/mock/data'

const num = (v: number) =>
  v.toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: v !== 0 && Math.abs(v) < 1 ? 6 : 2,
  })

/** Balances as a stepped list: venue, then the account it was held in, then
 *  the assets.
 *
 *  Flat, a desk running spot and perp on one exchange printed that exchange on
 *  every row — three `kucoin` lines that are one venue, two wallets and three
 *  holdings, and the reader has to do the grouping to see it. Stepped, the
 *  venue is said once and the shape of what is held under it is the shape of
 *  the list. */
export function BalanceTree() {
  const reported = BALANCES.filter((b) => b.reported)
  const venues = [...new Set(reported.map((b) => b.venue))]

  return (
    <div className="pb-2">
      {/* The two number columns are named once, at the top. A stepped list
          still has columns, and unlabelled ones make the reader guess which
          of the two is the free figure. */}
      <div className="sticky top-0 z-10 flex h-6 items-center bg-panel pr-3 pl-9 shadow-[inset_0_-1px_0_0_var(--color-line)]">
        <span className="col-head">Asset</span>
        <span className="col-head ml-auto w-32 text-right">Total</span>
        <span className="col-head w-32 text-right">Available</span>
      </div>
      {venues.map((venue) => {
        const rows = reported.filter((b) => b.venue === venue)
        const markets = [...new Set(rows.map((b) => b.market))]
        // Only the denomination is summed across wallets. Adding USDT to BTC
        // produces a figure nobody holds.
        const usdt = rows.filter((b) => b.asset === 'USDT').reduce((s, b) => s + b.total, 0)
        return (
          <div key={venue}>
            <div className="flex h-8 items-center gap-3 px-3 shadow-[inset_0_-1px_0_0_var(--color-line)]">
              <Venue venue={venue} className="text-sm" />
              <span className="num ml-auto text-sm text-fg">{num(usdt)}</span>
              <span className="w-12 text-right text-2xs text-fg-dim">USDT</span>
            </div>
            {markets.map((market) => (
              <div key={market}>
                <div className="flex h-5 items-center pl-9">
                  <span className="col-head">{market}</span>
                </div>
                {rows
                  .filter((b) => b.market === market)
                  .map((b) => (
                    <div key={`${b.account}:${b.asset}`} className="row flex items-center pr-3 pl-9">
                      <span className="text-sm text-fg-mid">{b.asset}</span>
                      <span className="num ml-auto w-32 text-right text-sm text-fg">
                        {num(b.total)}
                      </span>
                      {/* A venue that publishes a total and no free figure
                          leaves a dash. Repeating the total there invites
                          committing margin twice. */}
                      <span className="num w-32 text-right text-sm text-fg-mid">
                        {b.available === null ? (
                          <span
                            className="text-fg-dim"
                            title="this venue publishes no per-asset free figure"
                          >
                            —
                          </span>
                        ) : (
                          num(b.available)
                        )}
                      </span>
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
