/** The arbitrage boards, and why there are none.
 *
 *  This screen was designed in full against fixtures — three boards, venue
 *  filters, a countdown to the next settlement — and then found to have no
 *  endpoint behind it. The fixtures are gone. What they encoded about READING
 *  a board is kept in docs/UNBUILT.md, because that part was a design decision
 *  and not data.
 *
 *  It says what is missing rather than showing an empty table, because an empty
 *  table is a claim that the search ran and found nothing. */
export function ScannerPage() {
  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex flex-col gap-3 p-4">
        <span className="panel-title">Scanner</span>
        <p className="max-w-2xl text-sm text-fg-mid">
          raikiri publishes no arbitrage board. There is no endpoint behind this
          screen and no table in Postgres holding one — nothing has been
          computed and hidden; nothing computes it.
        </p>
        <p className="max-w-2xl text-sm text-fg-dim">
          The perp·perp board is the closest: every input except one is already
          on the wire. <span className="text-fg-mid">/v1/markets</span> names
          which venues list a symbol, and the{' '}
          <span className="text-fg-mid">quotes</span> and{' '}
          <span className="text-fg-mid">funding</span> stream channels carry the
          prices and the rates. The missing input is 24h volume, which nothing
          in the contract publishes — and depth is the filter that separates a
          route you can take from a number you can admire.
        </p>
        <p className="max-w-2xl text-sm text-fg-dim">
          The spot boards need spot gateways attached beside the perp ones. Same
          data, more accounts.
        </p>
      </section>
    </div>
  )
}
