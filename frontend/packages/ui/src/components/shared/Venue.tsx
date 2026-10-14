import { memo } from 'react'
import { cn } from '../../lib/format'
import { ExchangeIcon } from './ExchangeIcon'
import { useVenueBrand } from './VenueBrands'

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
  const brand = useVenueBrand(venue)
  return (
    <span className={cn('inline-flex items-center gap-2 align-middle', className)} title={brand?.name}>
      <ExchangeIcon venue={venue} className="size-[15px]" />
      <span className="text-fg-mid">{brand?.label ?? venue}</span>
      {market === 'spot' && <span className="text-2xs text-fg-dim">spot</span>}
    </span>
  )
})
