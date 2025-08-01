import { cn } from '@/lib/utils'

/** A state, written as a word.
 *
 *  This was a filled capsule. Five of them in four rows of a blotter is five
 *  coloured objects competing with the figures that are the point of the row,
 *  and the capsule adds nothing the word did not already say. A terminal
 *  writes `Long` in green and `Rejected` in red and leaves it at that.
 *
 *  Colour is spent only where the state carries direction or trouble.
 *  `accepted` is not good news and `cancelling` is not bad news — they are
 *  just where the order is. */
const tone: Record<string, string> = {
  // sides and direction
  buy: 'text-up', sell: 'text-down', long: 'text-up', short: 'text-down',
  // trouble, and completion
  filled: 'text-up', rejected: 'text-down', denied: 'text-down', dead: 'text-down',
  break: 'text-down',
  partially_filled: 'text-amber', triggered: 'text-amber',
  pending_update: 'text-amber', pending_cancel: 'text-amber',
  stale: 'text-amber', unknown: 'text-amber',
  agree: 'text-up',
}

const label: Record<string, string> = {
  partially_filled: 'Partial',
  pending_update: 'Amending',
  pending_cancel: 'Cancelling',
  partially: 'Partial',
}

const title = (s: string) => s.charAt(0).toUpperCase() + s.slice(1)

export function StatusBadge({ status, className }: { status: string; className?: string }) {
  return (
    <span className={cn(tone[status] ?? 'text-fg-mid', className)}>
      {label[status] ?? title(status.replace(/_/g, ' '))}
    </span>
  )
}
