import { memo } from 'react'
import { cn } from '../../lib/format'
import { ExchangeIcon } from './ExchangeIcon'

/** memo: board rows redraw on every tick and the logo never changes */
export const Venue = memo(function Venue({
  venue,
  market,
  className,
}: {
  venue: string
  /** shown only where a venue has both a spot and a perp account */
  market?: 'perp' | 'spot'
  className?: string
}) {
  return (
    <span className={cn('inline-flex items-center gap-2 align-middle', className)}>
      <ExchangeIcon venue={venue} className="size-[15px]" />
      <span className="text-fg-mid">{venue}</span>
      {market === 'spot' && <span className="text-2xs text-fg-dim">spot</span>}
    </span>
  )
})
