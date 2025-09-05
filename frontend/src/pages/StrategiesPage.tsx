/** The strategy list, and why it is empty.
 *
 *  admind knows a strategy only as an integer that rides on other rows. There
 *  is no registry to list, so this screen says that instead of listing
 *  fixtures. What the fixtures encoded about what a strategy must RECORD is
 *  kept in docs/UNBUILT.md. */
export function StrategiesPage() {
  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex flex-col gap-3 p-4">
        <span className="panel-title">Strategies</span>
        <p className="max-w-2xl text-sm text-fg-mid">
          There is no strategy registry to read. admind knows a strategy only as
          a numeric <span className="text-fg">strategy_id</span> carried on
          orders, positions and ledger rows — no name, no kind, no status, no
          start or stop time.
        </p>
        <p className="max-w-2xl text-sm text-fg-dim">
          What is running lives in strategyd, which publishes nothing to this
          API. Even counting per strategy is not expressible today:{' '}
          <span className="text-fg-mid">/v1/orders</span> and{' '}
          <span className="text-fg-mid">/v1/fills</span> accept no{' '}
          <span className="text-fg-mid">strategy_id</span> filter, and only{' '}
          <span className="text-fg-mid">/v1/positions</span> carries the field
          at all.
        </p>
      </section>
    </div>
  )
}
