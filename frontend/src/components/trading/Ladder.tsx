import { cn } from '@/lib/utils'

const price = (v: number) =>
  v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5)

/** Depth as a ladder with the size drawn behind the row.
 *
 *  The bar is cumulative, not per level: what a taker wants to know is how far
 *  down the book their order reaches, and a chart of individual levels answers
 *  a question nobody asked. */
export function Ladder({
  bids,
  asks,
}: {
  bids: { price: number; size: number }[]
  asks: { price: number; size: number }[]
}) {
  const cumulative = (rows: { price: number; size: number }[]) => {
    let running = 0
    return rows.map((r) => ({ ...r, depth: (running += r.size) }))
  }
  const bidRows = cumulative(bids)
  const askRows = cumulative(asks)
  const peak = Math.max(bidRows.at(-1)?.depth ?? 1, askRows.at(-1)?.depth ?? 1)

  const Side = ({
    rows,
    tone,
    align,
  }: {
    rows: { price: number; size: number; depth: number }[]
    tone: 'up' | 'down'
    align: 'left' | 'right'
  }) => (
    <div className="flex min-w-0 flex-1 flex-col">
      {rows.map((r) => (
        <div
          key={r.price}
          className="relative flex min-h-[18px] flex-1 items-center px-2 text-[11px]"
        >
          <span
            className={cn('absolute inset-y-px', tone === 'up' ? 'bg-up/12' : 'bg-down/12')}
            style={{ width: `${(r.depth / peak) * 100}%`, [align]: 0 }}
          />
          <span
            className={cn(
              'num relative z-10 flex w-full',
              align === 'left' ? 'flex-row-reverse' : 'flex-row',
              'justify-between',
            )}
          >
            <span className="text-fg-mid">{r.size.toFixed(3)}</span>
            <span className={tone === 'up' ? 'text-up' : 'text-down'}>{price(r.price)}</span>
          </span>
        </div>
      ))}
    </div>
  )

  return (
    <div className="flex min-h-0 flex-1 overflow-hidden py-1">
      <Side rows={bidRows} tone="up" align="right" />
      <div className="w-px shrink-0 bg-line" />
      <Side rows={askRows} tone="down" align="left" />
    </div>
  )
}
