import { useState } from 'react'
import { Link, useParams } from 'react-router'
import { ChartFrame, LinePlot } from '@/components/chart'
import { PanelHeader } from '@/components/shared/PanelHeader'
import { Venue } from '@/components/shared/Venue'
import { StatusBadge } from '@/components/shared/StatusBadge'
import { cn } from '@/lib/utils'
import {
  decompose,
  STRATEGY_DETAIL,
  type StrategyDetail,
  type StrategyLeg,
  type StrategySlot,
} from '@/mock/strategies'

const usd = (v: number, dp = 2) =>
  v.toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })
const signed = (v: number, dp = 2) => `${v >= 0 ? '+' : '−'}${usd(Math.abs(v), dp)}`

type Tab = 'fills' | 'funding' | 'orders'

/** One strategy, told so a report can be written from it.
 *
 *  The order of the screen is the order of the questions: what did it make and
 *  out of what, what is it holding, what was it configured to do, and then the
 *  two ledgers those three are derived from. */
export function StrategyPage() {
  const { name } = useParams()
  const detail = name === undefined ? undefined : STRATEGY_DETAIL[name]
  const [tab, setTab] = useState<Tab>('fills')
  const [running, setRunning] = useState<boolean | null>(null)

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

  // A strategy that has gone stale is still running — it just has not quoted.
  // Only `stopped` means the process is not working the market.
  const live = running ?? detail.status !== 'stopped'
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
            <StatusBadge status={live ? detail.status : 'stopped'} />
          </span>
          <span className="meta ml-auto">
            in trade {detail.heldHours.toFixed(1)}h · since {detail.since} UTC
          </span>
          {/* Stopping is the loud one, so it is the one that carries colour.
              A strategy that is running is the resting state and its control
              does not need to announce itself. */}
          <button
            type="button"
            onClick={() => setRunning(!live)}
            className={cn(
              'h-6 rounded-sm px-3 text-xs font-medium transition-colors duration-150',
              live
                ? 'bg-down/[0.14] text-down hover:bg-down/[0.22]'
                : 'bg-up/[0.14] text-up hover:bg-up/[0.22]',
            )}
          >
            {live ? 'Stop' : 'Start'}
          </button>
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
          {/* What it actually moved. A fee number without the notional behind
              it says nothing about whether the rate is any good. */}
          <Cell label="Volume" value={`${usd(p.volumeUsd, 0)} USDT`} tone={null} />
        </div>
      </section>

      <div className="grid shrink-0 grid-cols-1 gap-2 xl:grid-cols-[minmax(0,1fr)_320px]">
        <ChartFrame title="Cumulative" height={168}>
          {({ width, height }) => (
            <LinePlot
              x={detail.curve.hours}
              series={[
                { key: 'net', label: 'Net', color: 'var(--color-accent)', values: detail.curve.net, area: true },
                { key: 'funding', label: 'Funding', color: 'var(--color-up)', values: detail.curve.funding },
                { key: 'fees', label: 'Fees', color: 'var(--color-amber)', values: detail.curve.fees },
              ]}
              width={width}
              height={height}
            />
          )}
        </ChartFrame>
        <Params detail={detail} />
      </div>

      <Slots detail={detail} />

      <section className="panel flex min-h-0 flex-1 flex-col">
        <div className="flex h-7 shrink-0 items-center gap-5 px-3">
          {(['fills', 'orders', 'funding'] as const).map((t) => (
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
                {t === 'fills'
                  ? detail.fills.length
                  : t === 'orders'
                    ? detail.orders.length
                    : detail.funding.length}
              </span>
            </button>
          ))}
        </div>
        <div className="min-h-0 flex-1 overflow-auto">
          {tab === 'fills' && <Fills detail={detail} />}
          {tab === 'orders' && <Orders detail={detail} />}
          {tab === 'funding' && <Funding detail={detail} />}
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

/** The slots a strategy is running, compactly.
 *
 *  A slot is the unit the strategy thinks in, and for a hedge it holds two
 *  positions that only mean anything together — so the count in the header is
 *  the number of TRADES on, not the number of legs, and each slot is one block
 *  with its two sides on one line each.
 *
 *  The number between them is the convergence: how far the two venues have
 *  come together since entry. That, not either leg's own P&L, is what a hedge
 *  is waiting for. */
function Slots({ detail }: { detail: StrategyDetail }) {
  return (
    <section className="panel flex shrink-0 flex-col">
      <PanelHeader
        title="Slots"
        right={<span className="meta">{detail.slots.length}</span>}
      />
      <div>
        {detail.slots.map((slot) => (
          <Slot key={slot.index} slot={slot} />
        ))}
      </div>
    </section>
  )
}

function Slot({ slot }: { slot: StrategySlot }) {
  const hedged = slot.long !== null && slot.short !== null
  const entryBp =
    hedged && slot.long !== null && slot.short !== null
      ? ((slot.short.entry - slot.long.entry) / slot.long.entry) * 10_000
      : null
  const markBp =
    hedged && slot.long !== null && slot.short !== null
      ? ((slot.short.mark - slot.long.mark) / slot.long.mark) * 10_000
      : null
  const convergence = entryBp === null || markBp === null ? null : markBp - entryBp

  return (
    <div className="shadow-[inset_0_-1px_0_0_var(--color-line)] last:shadow-none">
      <div className="flex h-6 items-center gap-3 px-3">
        <span className="col-head">Slot {slot.index}</span>
        {convergence !== null && (
          <span className="ml-auto flex items-center gap-3 text-2xs">
            <span className="text-fg-dim">
              entry <span className="num text-fg-mid">{(entryBp as number).toFixed(1)}</span> →
              mark <span className="num text-fg-mid">{(markBp as number).toFixed(1)}</span> bp
            </span>
            <span
              className={cn('num font-medium', convergence >= 0 ? 'text-up' : 'text-down')}
            >
              {signed(convergence, 1)} bp
            </span>
          </span>
        )}
      </div>
      {slot.long !== null && <LegRow leg={slot.long} />}
      {slot.short !== null && <LegRow leg={slot.short} />}
    </div>
  )
}

function LegRow({ leg }: { leg: StrategyLeg }) {
  const pnl = (leg.mark - leg.entry) * leg.qty * (leg.side === 'long' ? 1 : -1)
  return (
    <div className="row flex items-center gap-3 px-3 text-sm">
      <span
        className={cn(
          'w-12 shrink-0 text-xs font-medium capitalize',
          leg.side === 'long' ? 'text-up' : 'text-down',
        )}
      >
        {leg.side}
      </span>
      <span className="w-28 shrink-0">
        <Venue venue={leg.venue} />
      </span>
      <span className="w-24 shrink-0 text-fg">{leg.symbol}</span>
      <span
        className={cn('num w-16 shrink-0 text-right', leg.side === 'long' ? 'text-up' : 'text-down')}
      >
        {leg.side === 'long' ? '+' : '−'}
        {leg.qty}
      </span>
      <span className="num w-24 shrink-0 text-right text-fg-mid">{usd(leg.entry)}</span>
      <span className="num w-24 shrink-0 text-right text-fg">{usd(leg.mark)}</span>
      <span className={cn('num w-24 shrink-0 text-right', pnl >= 0 ? 'text-up' : 'text-down')}>
        {signed(pnl)}
      </span>
      <span className="ml-auto flex items-baseline gap-4 text-xs">
        <span className="text-fg-dim">
          funding{' '}
          <span className={cn('num', leg.fundingUsd >= 0 ? 'text-up' : 'text-down')}>
            {signed(leg.fundingUsd)}
          </span>
        </span>
        <span className="text-fg-dim">
          fees{' '}
          <span className={cn('num', -leg.feesUsd >= 0 ? 'text-up' : 'text-fg-mid')}>
            {signed(-leg.feesUsd)}
          </span>
        </span>
      </span>
    </div>
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
          <th className="col-head px-3 text-left">ID</th>
          <th className="col-head px-3 text-left">Time</th>
          <th className="col-head px-3 text-left">Slot</th>
          <th className="col-head px-3 text-left">Venue</th>
          <th className="col-head px-3 text-left">Symbol</th>
          <th className="col-head px-3 text-left">Side</th>
          <th className="col-head px-3 text-right">Price</th>
          <th className="col-head px-3 text-right">Qty</th>
          <th className="col-head px-3 text-left">Type</th>
          <th className="col-head px-3 text-right">Fee</th>
          <th className="col-head px-3 text-left">Order</th>
          <th className="col-head px-3 text-left">Venue exec</th>
        </tr>
      </thead>
      <tbody>
        {detail.fills.map((f) => (
          <tr key={f.id} className="row">
            <td className="num px-3 text-xs text-fg-dim">{f.id}</td>
            <td className="num px-3 text-fg-mid">{f.at}</td>
            <td className="num px-3 text-fg-dim">{f.slot}</td>
            <td className="px-3">
              <Venue venue={f.venue} />
            </td>
            <td className="px-3 text-fg">{f.symbol}</td>
            <td className="px-3">
              <StatusBadge status={f.side} />
            </td>
            <td className="num px-3 text-right text-fg">{usd(f.price)}</td>
            <td className="num px-3 text-right text-fg">{f.qty}</td>
            {/* Maker and taker differ by an order of magnitude in what they
                cost, so the flag travels with every fill rather than being
                blended into an average nobody can take apart. */}
            <td className="px-3 text-fg-mid capitalize">{f.liquidity}</td>
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
            <td className="px-3">
              <Venue venue={f.venue} />
            </td>
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

function Orders({ detail }: { detail: StrategyDetail }) {
  if (detail.orders.length === 0) {
    return <p className="px-3 py-6 text-xs text-fg-dim">Nothing working.</p>
  }
  return (
    <table className="w-full border-collapse text-sm whitespace-nowrap">
      <thead className="sticky top-0 z-10 bg-panel">
        <tr className="h-6 shadow-[inset_0_-1px_0_0_var(--color-line)]">
          <th className="col-head px-3 text-left">ID</th>
          <th className="col-head px-3 text-left">Slot</th>
          <th className="col-head px-3 text-left">Venue</th>
          <th className="col-head px-3 text-left">Symbol</th>
          <th className="col-head px-3 text-left">Side</th>
          <th className="col-head px-3 text-left">Type</th>
          <th className="col-head px-3 text-right">Price</th>
          <th className="col-head px-3 text-right">Filled</th>
          <th className="col-head px-3 text-left">Status</th>
          <th className="col-head px-3 text-right">Placed</th>
        </tr>
      </thead>
      <tbody>
        {detail.orders.map((o) => (
          <tr key={o.id} className="row">
            <td className="num px-3 text-xs text-fg-dim">{o.id}</td>
            <td className="num px-3 text-fg-dim">{o.slot}</td>
            <td className="px-3">
              <Venue venue={o.venue} />
            </td>
            <td className="px-3 text-fg">{o.symbol}</td>
            <td className="px-3">
              <StatusBadge status={o.side} />
            </td>
            <td className="px-3 text-fg-mid capitalize">{o.type}</td>
            <td className="num px-3 text-right text-fg">{usd(o.price)}</td>
            <td className="num px-3 text-right text-fg">
              <span className={o.filled > 0 ? 'text-amber' : 'text-fg-dim'}>{o.filled}</span>
              <span className="text-fg-dim"> / {o.qty}</span>
            </td>
            <td className="px-3">
              <StatusBadge status={o.status} />
            </td>
            <td className="num px-3 text-right text-xs text-fg-mid">{o.placedAt}</td>
          </tr>
        ))}
      </tbody>
    </table>
  )
}
