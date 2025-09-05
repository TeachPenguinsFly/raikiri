import { useParams } from 'react-router'

/** One strategy's page, and why it cannot be drawn.
 *
 *  The deepest of the three: it needs run parameters, a slot structure and
 *  per-slot settlements, none of which any process publishes. The slot model
 *  and the report contract it was designed against are kept in
 *  docs/UNBUILT.md.
 *
 *  There is no Start/Stop here. There was, and it was removed rather than
 *  disabled: a greyed control still says the feature exists, and a second
 *  producer into the command rings is a decision to be made deliberately
 *  rather than a POST added quietly. */
export function StrategyPage() {
  const { name } = useParams()
  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel flex flex-col gap-3 p-4">
        <span className="panel-title">{name ?? 'Strategy'}</span>
        <p className="max-w-2xl text-sm text-fg-mid">
          Nothing is published about a strategy beyond the{' '}
          <span className="text-fg">strategy_id</span> stamped on the rows it
          produced. No run parameters, no slot structure, no funding
          settlements attributed to a slot.
        </p>
        <p className="max-w-2xl text-sm text-fg-dim">
          The rows it produced ARE readable — orders, fills and positions carry
          the id — but only <span className="text-fg-mid">/v1/positions</span>{' '}
          can be filtered by it, so this page would show a third of a strategy
          and imply the rest.
        </p>
      </section>
    </div>
  )
}
