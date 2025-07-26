import { cn } from '@/lib/utils'
import { ExchangeIcon } from './ExchangeIcon'

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
    <span className={cn('inline-flex items-center gap-1.5 align-middle', className)}>
      <ExchangeIcon venue={venue} className="size-[15px]" />
      {full === true && <span className="text-2xs font-semibold text-fg-mid">{venue}</span>}
    </span>
  )
}
