import { memo, useMemo } from 'react'
import { cn } from '@raikiri/ui'
import { Empty, n } from '@raikiri/ui/v3'
import { useTrades } from '../api/stream'
import { px, qty } from '../lib/market'
import { useTerminal } from './state'

interface Print {
  seq: number
  price: number
  size: number
  up: boolean
  big: boolean
  time: string
}

const k_big = 5

const clock = (nanos: string) => new Date(Number(nanos) / 1e6).toISOString().slice(11, 19)

const Row = memo(function Row({ price, size, up, big, time, places }: Print & { places: number }) {
  return (
    <div
      className={cn(
        'grid h-[22px] shrink-0 grid-cols-[1.2fr_1fr_1fr] items-center px-3 text-xs',
        big && (up ? 'bg-up/[0.07]' : 'bg-down/[0.07]'),
      )}
    >
      <span className={cn('mono', up ? 'text-up' : 'text-down', big && 'font-semibold')}>{px(price, places)}</span>
      <span className={cn('mono text-right', big ? 'font-medium text-fg' : 'text-fg-mid')}>{qty(size)}</span>
      <span className="mono text-right text-fg-dim">{time}</span>
    </div>
  )
})

export function TradesWidget() {
  const { pair, leg } = useTerminal()
  const sized = leg?.sized === true
  const trades = useTrades(sized ? leg.id : undefined)
  const unit = leg?.unit ?? 1

  const prints = useMemo(() => {
    const out: Print[] = []
    // oldest first, so a print with no side takes its colour from the move that led to it
    let last: number | undefined
    let up = true
    for (let i = trades.length - 1; i >= 0; i--) {
      const t = trades[i]!
      const price = n(t.price)
      const size = n(t.size)
      if (price === undefined || size === undefined) continue
      if (t.side !== undefined) up = t.side === 'buy'
      else if (last !== undefined && price !== last) up = price > last
      last = price
      out.push({ seq: t.seq, price, size: size * unit, up, big: false, time: clock(t.ts_event) })
    }
    const sizes = out.map((p) => p.size).sort((a, b) => a - b)
    const median = sizes[sizes.length >> 1] ?? 0
    for (const p of out) p.big = out.length >= 10 && p.size >= median * k_big
    return out.reverse()
  }, [trades, unit])

  if (pair === undefined) return <Empty title="Pick a market" />

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="col-head grid shrink-0 grid-cols-[1.2fr_1fr_1fr] px-3 py-1">
        <span>Price</span>
        <span className="text-right">Size</span>
        <span className="text-right">Time</span>
      </div>
      {prints.length === 0 ? (
        <Empty title="No trades yet" />
      ) : (
        <div className="flex min-h-0 flex-1 flex-col overflow-y-auto [scrollbar-width:thin]">
          {prints.map((p) => (
            <Row key={p.seq} {...p} places={leg?.places ?? 2} />
          ))}
        </div>
      )}
    </div>
  )
}
