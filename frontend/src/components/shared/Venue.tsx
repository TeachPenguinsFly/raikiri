import { cn } from '@/lib/utils'
import { ExchangeIcon } from './ExchangeIcon'

/** A venue, named the same way everywhere.
 *
 *  The bare word was the problem: `bybit` set as body text in a column of
 *  numbers reads as a value rather than as an identity, and the reader has to
 *  parse it every row. The mark carries the identity and the word confirms it,
 *  which is one glance instead of one read. */
export function Venue({
  venue,
  market,
  className,
}: {
  venue: string
  /** Shown only where a venue can appear twice — a desk running spot and perp
   *  on one exchange has two accounts, and the row has to say which. */
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
