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
          API. The number itself is not missing:{' '}
          <span className="text-fg-mid">Order</span>,{' '}
          <span className="text-fg-mid">Position</span> and{' '}
          <span className="text-fg-mid">PnlIncrement</span> all carry it, and a
          fill's owner is recoverable from its{' '}
          <span className="text-fg-mid">client_order_id</span>, which packs{' '}
          <span className="text-fg">{'{strategy_id|seq}'}</span> — the manual
          desk holding a reserved id so a person's order is distinguishable from
          a strategy's. What is missing is a filter: only{' '}
          <span className="text-fg-mid">/v1/positions</span> takes{' '}
          <span className="text-fg-mid">strategy_id</span> as a parameter, so
          counting per strategy means reading every row and unpacking it.
        </p>
      </section>
    </div>
  )
}
