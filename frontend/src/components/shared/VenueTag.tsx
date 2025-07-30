import { cn } from '@/lib/utils'
import { ExchangeIcon } from './ExchangeIcon'

/** A venue, named quietly.
 *
 *  The mark identifies it and the label confirms it, so the label is set at
 *  secondary weight — the venue column is how you find a row, not what you
 *  read off it. */
export function VenueTag({
  venue,
  full,
  className,
}: {
  venue: string
  full?: boolean
  className?: string
}) {
  return (
    <span className={cn('inline-flex items-center gap-2 align-middle', className)}>
      <ExchangeIcon venue={venue} className="size-4" />
      {full === true && <span className="text-sm text-fg-mid">{venue}</span>}
    </span>
  )
}
