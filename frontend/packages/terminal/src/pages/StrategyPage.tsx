import { useParams } from 'react-router'

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
