import { memo } from 'react'
import type { components } from '@/api/types'
import { cn, midPrice, placesOf, price, size, spreadBp } from '@/lib/format'

type Book = components['schemas']['Book']

type Level = { price: string; size: number; depth: number }

/** an unparseable size counts as zero so depth stays monotonic */
function cumulative(levels: Book['bids']): Level[] {
  let running = 0
  return levels.map((l) => {
    const qty = Number(l.size) || 0
    return { price: l.price, size: qty, depth: (running += qty) }
  })
}

export const Ladder = memo(function Ladder({ book }: { book: Book }) {
  const bids = cumulative(book.bids)
  const asks = cumulative(book.asks)
  const peak = Math.max(bids.at(-1)?.depth ?? 1, asks.at(-1)?.depth ?? 1)
  const places = placesOf([...bids, ...asks].map((l) => l.price))
  const bid = book.bids[0]?.price
  const ask = book.asks[0]?.price
  const spread = bid === undefined || ask === undefined ? null : spreadBp(Number(bid), Number(ask))

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex h-5 shrink-0 items-center justify-between px-3">
        <span className="col-head">Price</span>
        <span className="col-head">Size</span>
        <span className="col-head">Total</span>
      </div>

      <div className="flex min-h-0 flex-1 flex-col-reverse justify-start overflow-hidden">
        {asks.map((r, i) => (
          <Row key={r.price} row={r} places={places} peak={peak} tone="down" faded={i} />
        ))}
      </div>

      <div className="flex h-7 shrink-0 items-center justify-between bg-raised px-3">
        <span className="num text-sm font-medium text-fg">
          {bid === undefined || ask === undefined ? '—' : midPrice(bid, ask)}
        </span>
        <span className="num text-2xs text-fg-dim">
          {spread === null ? '—' : `${spread.toFixed(1)} bp`}
        </span>
      </div>

      <div className="flex min-h-0 flex-1 flex-col overflow-hidden">
        {bids.map((r, i) => (
          <Row key={r.price} row={r} places={places} peak={peak} tone="up" faded={i} />
        ))}
      </div>
    </div>
  )
})

function Row({
  row,
  places,
  peak,
  tone,
  faded,
}: {
  row: Level
  places: number
  peak: number
  tone: 'up' | 'down'
  faded: number
}) {
  return (
    <div className="relative flex min-h-[14px] flex-1 items-center px-3 text-[11px] leading-none transition-colors duration-150 hover:bg-hover">
      <span
        className={cn('absolute inset-y-0 right-0', tone === 'up' ? 'bg-up/[0.08]' : 'bg-down/[0.08]')}
        style={{ width: `${(row.depth / peak) * 100}%` }}
      />
      <span className="num relative z-10 flex w-full justify-between">
        <span
          className={cn('w-[62px]', tone === 'up' ? 'text-up' : 'text-down')}
          style={faded === 0 ? undefined : { opacity: Math.max(0.45, 0.8 - faded * 0.02) }}
        >
          {price(row.price, places)}
        </span>
        <span className="flex-1 text-right text-fg-mid">{size(row.size)}</span>
        <span className="w-[52px] text-right text-fg-dim">{size(row.depth)}</span>
      </span>
    </div>
  )
}
