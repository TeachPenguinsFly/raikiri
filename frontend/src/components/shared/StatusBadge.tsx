import { cn } from '@/lib/utils'

/** colour only for direction or trouble */
const tone: Record<string, string> = {
  // sides and direction
  buy: 'text-up', sell: 'text-down', long: 'text-up', short: 'text-down',
  // trouble, and completion
  filled: 'text-up', rejected: 'text-down', denied: 'text-down', dead: 'text-down',
  break: 'text-down',
  partially_filled: 'text-amber', triggered: 'text-amber',
  pending_update: 'text-amber', pending_cancel: 'text-amber',
  stale: 'text-amber', unknown: 'text-amber', stopped: 'text-fg-dim',
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
