import { useState } from 'react'
import { Link, useParams } from 'react-router'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { cn } from '@/lib/utils'
import { decompose, STRATEGY_DETAIL, type StrategyDetail } from '@/mock/strategies'

const usd = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })
const signed = (v: number, dp = 2) => `${v >= 0 ? '+' : '−'}${usd(Math.abs(v), dp)}`

type Tab = 'fills' | 'funding'

/** One strategy, told so a report can be written from it.
 *
 *  The order of the screen is the order of the questions: what did it make and
 *  out of what, what is it holding, what was it configured to do, and then the
 *  two ledgers those three are derived from. */
export function StrategyPage() {
  const { name } = useParams()
  const detail = name === undefined ? undefined : STRATEGY_DETAIL[name]
  const [tab, setTab] = useState<Tab>('fills')

  if (detail === undefined) {
    return (
      <div className="panel flex h-full items-start p-3 text-xs text-fg-dim">
        No strategy named {name}.{' '}
        <Link to="/strategies" className="ml-1 text-accent">
          Back to the list
        </Link>
      </div>
    )
  }

  const p = decompose(detail)

  return (
    <div className="flex h-full min-h-0 flex-col gap-2">
      <section className="panel shrink-0">
        <div className="flex h-9 items-center gap-3 px-3">
          <Link to="/strategies" className="text-xs text-fg-dim hover:text-fg-mid">
            Strategies
          </Link>
          <span className="text-fg-dim">/</span>
          <span className="text-sm font-semibold tracking-[-0.01em] text-fg">{detail.name}</span>
          <span className="text-2xs text-fg-dim uppercase">{detail.kind}</span>
          <span className="text-xs">
            <StatusBadge status={detail.status} />
          </span>
          <span className="meta ml-auto">
            in trade {detail.heldHours.toFixed(1)}h · since {detail.since} UTC
          </span>
        </div>

        {/* The decomposition, not a single figure. A net number that cannot be
            split into price, funding and fees is a number nobody can act on:
            a carry that is winning on funding and losing more on fees is a
            different problem from one whose legs have drifted apart. */}
        <div className="grid grid-cols-2 gap-px bg-line lg:grid-cols-5">
          <Cell label="Unrealized" value={signed(p.unrealized)} tone={p.unrealized} />
          <Cell label="Funding" value={signed(p.funding)} tone={p.funding} />
          {/* A negative fee is a maker rebate and stays negative. Folding it
              into a "cost" hides that a quoter earns its spread there. */}
          <Cell label="Fees" value={signed(-p.fees)} tone={-p.fees} />
          <Cell label="Net" value={signed(p.net)} tone={p.net} strong />
          <Cell
            label="Liquidity"
            value={`${p.makerFills} maker · ${p.takerFills} taker`}
            tone={null}
          />
        </div>
      </section>

      <div className="grid shrink-0 grid-cols-1 gap-2 xl:grid-cols-[minmax(0,1fr)_320px]">
        <Legs detail={detail} />
        <Params detail={detail} />
      </div>

      <section className="panel flex min-h-0 flex-1 flex-col">
        <div className="flex h-7 shrink-0 items-center gap-5 px-3">
          {(['fills', 'funding'] as const).map((t) => (
            <button
              key={t}
              type="button"
              onClick={() => setTab(t)}
              className={cn(
                'relative flex h-7 items-center gap-1.5 text-xs capitalize transition-colors duration-150',
                tab === t
                  ? 'font-medium text-fg after:absolute after:inset-x-0 after:bottom-0 after:h-[2px] after:rounded-t-full after:bg-accent'
                  : 'text-fg-dim hover:text-fg-mid',
              )}
            >
              {t}
              <span className="num text-2xs text-fg-dim">
                {t === 'fills' ? detail.fills.length : detail.funding.length}
              </span>
            </button>
          ))}
        </div>
        <div className="min-h-0 flex-1 overflow-auto">
          {tab === 'fills' ? <Fills detail={detail} /> : <Funding detail={detail} />}
        </div>
      </section>
    </div>
  )
}

function Cell({
  label,
  value,
  tone,
  strong,
}: {
  label: string
  value: string
  tone: number | null
  strong?: boolean
}) {
  return (
    <div className="bg-panel px-3 py-2">
      <div className="col-head">{label}</div>
      <div
        className={cn(
          'num mt-0.5 text-sm',
          strong === true && 'font-semibold',
          tone === null ? 'text-fg' : tone >= 0 ? 'text-up' : 'text-down',
        )}
      >
        {value}
      </div>
    </div>
  )
}

function Legs({ detail }: { detail: StrategyDetail }) {
  return (
    <section className="panel flex flex-col">
      <PanelHeader
        title="Legs"
        right={<span className="meta">{detail.legs.length}</span>}
      />
      <table className="w-full border-collapse text-sm whitespace-nowrap">
        <thead>
          <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
            <th className="col-head px-3 text-left">Slot</th>
            <th className="col-head px-3 text-left">Venue</th>
            <th className="col-head px-3 text-left">Symbol</th>
            <th className="col-head px-3 text-left">Side</th>
            <th className="col-head px-3 text-right">Qty</th>
            <th className="col-head px-3 text-right">Entry</th>
            <th className="col-head px-3 text-right">Mark</th>
            <th className="col-head px-3 text-right">Funding</th>
            <th className="col-head px-3 text-right">Fees</th>
          </tr>
        </thead>
        <tbody>
          {detail.legs.map((l) => (
            <tr key={l.slot} className="row">
              {/* The slot is the strategy's own handle for the leg — the intent
                  ring, the fills and the reconciler all key on it, so a report
                  that omits it cannot be tied back to anything. */}
              <td className="num px-3 text-fg-dim">{l.slot}</td>
              <td className="px-3 text-fg-mid">{l.venue}</td>
              <td className="px-3 text-fg">{l.symbol}</td>
              <td className="px-3">
                <StatusBadge status={l.side} />
              </td>
              <td className={cn('num px-3 text-right', l.side === 'long' ? 'text-up' : 'text-down')}>
                {l.side === 'long' ? '+' : '−'}
                {l.qty}
              </td>
              <td className="num px-3 text-right text-fg-mid">{usd(l.entry)}</td>
              <td className="num px-3 text-right text-fg">{usd(l.mark)}</td>
              <td className={cn('num px-3 text-right', l.fundingUsd >= 0 ? 'text-up' : 'text-down')}>
                {signed(l.fundingUsd)}
              </td>
              <td className={cn('num px-3 text-right', l.feesUsd <= 0 ? 'text-up' : 'text-fg-mid')}>
                {signed(-l.feesUsd)}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  )
}

/** What the run was started with.
 *
 *  A P&L reported without its parameters is a result nobody can reproduce or
 *  attribute — the same strategy at a two basis point offset and at a five is
 *  two different strategies. */
function Params({ detail }: { detail: StrategyDetail }) {
  const p = detail.params
  const rows: Array<[string, string]> = []
  if (p.entryBp !== undefined) rows.push(['Entry threshold', `${p.entryBp.toFixed(1)} bp`])
  if (p.exitBp !== undefined) rows.push(['Exit threshold', `${p.exitBp.toFixed(1)} bp`])
  if (p.quoteOffsetBp !== undefined) rows.push(['Quote offset', `${p.quoteOffsetBp.toFixed(1)} bp`])
  if (p.requoteMs !== undefined) rows.push(['Requote interval', `${p.requoteMs} ms`])
  if (p.maxInventory !== undefined) rows.push(['Max inventory', String(p.maxInventory)])
  if (p.skewBpPerUnit !== undefined) rows.push(['Skew', `${p.skewBpPerUnit} bp per unit`])
  rows.push(['Max notional', `${usd(p.maxNotionalUsd, 0)} USDT`])
  rows.push(['Max slippage', `${p.maxSlippageBp.toFixed(1)} bp`])
  if (p.maxHoldHours !== undefined) rows.push(['Max hold', `${p.maxHoldHours} h`])

  return (
    <section className="panel flex flex-col">
      <PanelHeader title="Parameters" />
      <dl className="px-3 pb-2">
        {rows.map(([term, value]) => (
          <div key={term} className="flex h-6 items-baseline justify-between gap-4">
            <dt className="text-xs text-fg-dim">{term}</dt>
            <dd className="num text-xs text-fg">{value}</dd>
          </div>
        ))}
      </dl>
    </section>
  )
}

function Fills({ detail }: { detail: StrategyDetail }) {
  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          <th className="col-head px-3 text-left">Time</th>
          <th className="col-head px-3 text-left">Slot</th>
          <th className="col-head px-3 text-left">Venue</th>
          <th className="col-head px-3 text-left">Symbol</th>
          <th className="col-head px-3 text-left">Side</th>
          <th className="col-head px-3 text-right">Price</th>
          <th className="col-head px-3 text-right">Qty</th>
          <th className="col-head px-3 text-left">Liquidity</th>
          <th className="col-head px-3 text-right">Fee</th>
          <th className="col-head px-3 text-left">Order</th>
          <th className="col-head px-3 text-left">Execution</th>
        </tr>
      </thead>
      <tbody>
        {detail.fills.map((f) => (
          <tr key={f.execId} className="row">
            <td className="num px-3 text-fg-mid">{f.at}</td>
            <td className="num px-3 text-fg-dim">{f.slot}</td>
            <td className="px-3 text-fg-mid">{f.venue}</td>
            <td className="px-3 text-fg">{f.symbol}</td>
            <td className="px-3">
              <StatusBadge status={f.side} />
            </td>
            <td className="num px-3 text-right text-fg">{usd(f.price)}</td>
            <td className="num px-3 text-right text-fg">{f.qty}</td>
            {/* Maker and taker differ by an order of magnitude in what they
                cost, so the flag travels with every fill rather than being
                blended into an average nobody can take apart. */}
            <td className="px-3 text-fg-mid">{f.liquidity}</td>
            <td className={cn('num px-3 text-right', f.fee < 0 ? 'text-up' : 'text-fg-mid')}>
              {f.fee < 0 ? `+${usd(-f.fee, 4)}` : usd(f.fee, 4)}
              <span className="ml-1 text-2xs text-fg-dim">{f.feeAsset}</span>
            </td>
            <td className="num px-3 text-xs text-fg-dim">{f.orderId}</td>
            <td className="num px-3 text-xs text-fg-dim">{f.execId}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}

function Funding({ detail }: { detail: StrategyDetail }) {
  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          <th className="col-head px-3 text-left">Settled</th>
          <th className="col-head px-3 text-left">Slot</th>
          <th className="col-head px-3 text-left">Venue</th>
          <th className="col-head px-3 text-left">Symbol</th>
          <th className="col-head px-3 text-right">Rate</th>
          <th className="col-head px-3 text-right">Every</th>
          <th className="col-head px-3 text-right">Position</th>
          <th className="col-head px-3 text-right">Mark</th>
          <th className="col-head px-3 text-right">Amount</th>
        </tr>
      </thead>
      <tbody>
        {detail.funding.map((f) => (
          <tr key={`${f.at}:${f.slot}`} className="row">
            <td className="num px-3 text-fg-mid">{f.at}</td>
            <td className="num px-3 text-fg-dim">{f.slot}</td>
            <td className="px-3 text-fg-mid">{f.venue}</td>
            <td className="px-3 text-fg">{f.symbol}</td>
            {/* The venue's own rate for that settlement, not one read off a
                ticker near the minute. The two disagree, and the disagreement
                would land in an append-only ledger. */}
            <td className="num px-3 text-right text-fg">{(f.rate * 100).toFixed(4)}%</td>
            <td className="num px-3 text-right text-fg-dim">{f.intervalHours}h</td>
            <td className={cn('num px-3 text-right', f.positionQty >= 0 ? 'text-up' : 'text-down')}>
              {f.positionQty > 0 ? '+' : ''}
              {f.positionQty}
            </td>
            <td className="num px-3 text-right text-fg-mid">{usd(f.mark)}</td>
            <td className={cn('num px-3 text-right', f.amountUsd >= 0 ? 'text-up' : 'text-down')}>
              {signed(f.amountUsd)}
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}
