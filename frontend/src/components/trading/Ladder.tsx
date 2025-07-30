import { cn } from '@/lib/utils'

const price = (v: number) =>
  v >= 1000 ? v.toFixed(1) : v >= 1 ? v.toFixed(3) : v.toFixed(5)

interface Level {
  price: number
  size: number
}

/** Depth as two ladders around the touch.
 *
 *  The depth bar is CUMULATIVE, not per level: what a taker wants to know is
 *  how far down the book their order reaches, and a chart of individual levels
 *  answers a question nobody asked.
 *
 *  The bars are drawn at 9% opacity. A book whose backgrounds are as saturated
 *  as its prices turns the whole column into a colour field, and then the one
 *  thing the column exists for — which price, at what size — is the hardest
 *  thing on it to read. */
export function Ladder({ bids, asks }: { bids: Level[]; asks: Level[] }) {
  const cumulative = (rows: Level[]) => {
    let running = 0
    return rows.map((r) => ({ ...r, depth: (running += r.size) }))
  }
  const bidRows = cumulative(bids)
  const askRows = cumulative(asks)
  const peak = Math.max(bidRows.at(-1)?.depth ?? 1, askRows.at(-1)?.depth ?? 1)
  const spread =
    bids[0] !== undefined && asks[0] !== undefined ? asks[0].price - bids[0].price : null
  const mid =
    bids[0] !== undefined && asks[0] !== undefined ? (asks[0].price + bids[0].price) / 2 : null

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      {/* Each side labels its own columns, so the header sits over the figures
          it names rather than over the gap between them. */}
      <div className="flex shrink-0 pb-1">
        <div className="flex flex-1 justify-between px-3">
          <span className="col-head">Size</span>
          <span className="col-head">Bid</span>
        </div>
        <div className="flex flex-1 justify-between px-3">
          <span className="col-head">Ask</span>
          <span className="col-head">Size</span>
        </div>
      </div>

      <div className="flex min-h-0 flex-1 overflow-y-auto">
        <Side rows={bidRows} peak={peak} tone="up" align="right" />
        <Side rows={askRows} peak={peak} tone="down" align="left" />
      </div>

      {/* The inside market, stated once. Reading it off the top two rows means
          doing arithmetic on every glance. */}
      <div className="flex h-9 shrink-0 items-center justify-between px-3">
        <span className="num text-sm text-fg">{mid === null ? '—' : price(mid)}</span>
        <span className="num text-2xs text-fg-dim">
          {spread === null || mid === null
            ? '—'
            : `${((spread / mid) * 10_000).toFixed(1)} bp spread`}
        </span>
      </div>
    </div>
  )
}

function Side({
  rows,
  peak,
  tone,
  align,
}: {
  rows: Array<Level & { depth: number }>
  peak: number
  tone: 'up' | 'down'
  align: 'left' | 'right'
}) {
  return (
    <div className="flex min-w-0 flex-1 flex-col">
      {rows.map((r, i) => (
        <div
          key={r.price}
          className={cn(
            'relative flex min-h-[17px] flex-1 items-center px-3 text-xs transition-colors duration-150',
            'hover:bg-hover',
          )}
        >
          <span
            className={cn('absolute inset-y-0', tone === 'up' ? 'bg-up/[0.09]' : 'bg-down/[0.09]')}
            style={{ width: `${(r.depth / peak) * 100}%`, [align]: 0 }}
          />
          <span
            className={cn(
              'num relative z-10 flex w-full justify-between',
              align === 'left' ? 'flex-row-reverse' : 'flex-row',
            )}
          >
            <span className="text-fg-dim">{r.size.toFixed(3)}</span>
            {/* Only the touch is at full strength, and the rest fade with
                depth. Forty saturated figures turn the book into a colour
                field, and then the one thing it exists for — which price, at
                what size — is the hardest thing on it to read. */}
            <span
              className={tone === 'up' ? 'text-up' : 'text-down'}
              style={i === 0 ? undefined : { opacity: Math.max(0.42, 0.78 - i * 0.02) }}
            >
              {price(r.price)}
            </span>
          </span>
        </div>
      ))}
    </div>
  )
}
