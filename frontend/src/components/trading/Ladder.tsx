import { size } from '@/lib/format'
import { cn } from '@/lib/utils'

const price = (v: number) => (v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5))

interface Level {
  price: number
  size: number
}

/** asks above, bids below; depth bars are cumulative from the touch */
export function Ladder({ bids, asks }: { bids: Level[]; asks: Level[] }) {
  const cumulative = (rows: Level[]) => {
    let running = 0
    return rows.map((r) => ({ ...r, depth: (running += r.size) }))
  }
  const bidRows = cumulative(bids)
  const askRows = cumulative(asks)
  const peak = Math.max(bidRows.at(-1)?.depth ?? 1, askRows.at(-1)?.depth ?? 1)
  const spread = bids[0] !== undefined && asks[0] !== undefined ? asks[0].price - bids[0].price : null
  const mid = bids[0] !== undefined && asks[0] !== undefined ? (asks[0].price + bids[0].price) / 2 : null

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex h-5 shrink-0 items-center justify-between px-3">
        <span className="col-head">Price</span>
        <span className="col-head">Size</span>
        <span className="col-head">Total</span>
      </div>

      <div className="flex min-h-0 flex-1 flex-col-reverse justify-start overflow-hidden">
        {askRows.map((r, i) => (
          <Row key={r.price} row={r} peak={peak} tone="down" faded={i} />
        ))}
      </div>

      <div className="flex h-7 shrink-0 items-center justify-between bg-raised px-3">
        <span className="num text-sm font-medium text-fg">{mid === null ? '—' : price(mid)}</span>
        <span className="num text-2xs text-fg-dim">
          {spread === null || mid === null ? '—' : `${((spread / mid) * 10_000).toFixed(1)} bp`}
        </span>
      </div>

      <div className="flex min-h-0 flex-1 flex-col overflow-hidden">
        {bidRows.map((r, i) => (
          <Row key={r.price} row={r} peak={peak} tone="up" faded={i} />
        ))}
      </div>
    </div>
  )
}

function Row({
  row,
  peak,
  tone,
  faded,
}: {
  row: Level & { depth: number }
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
          {price(row.price)}
        </span>
        <span className="flex-1 text-right text-fg-mid">{size(row.size)}</span>
        <span className="w-[52px] text-right text-fg-dim">{size(row.depth)}</span>
      </span>
    </div>
  )
}
