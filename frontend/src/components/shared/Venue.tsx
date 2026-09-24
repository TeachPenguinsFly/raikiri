import { cn } from '@/lib/utils'
import { ExchangeIcon } from './ExchangeIcon'

export function Venue({
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
}
