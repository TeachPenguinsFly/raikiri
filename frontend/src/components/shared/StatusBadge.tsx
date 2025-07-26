import { Badge, type BadgeTone } from './Badge'

/** One vocabulary for every state the terminal shows, so `filled` is the same
 *  green on an order blotter and on a fill tape. A status with no entry reads
 *  neutral rather than inventing a colour for a word nobody mapped. */
const tone: Record<string, BadgeTone> = {
  // orders
  initialized: 'outline', submitted: 'accent', accepted: 'accent',
  partially_filled: 'amber', filled: 'up', canceled: 'neutral',
  rejected: 'down', denied: 'down', expired: 'neutral', triggered: 'amber',
  pending_update: 'amber', pending_cancel: 'amber',
  // reconciliation
  agree: 'up', break: 'down', unknown: 'amber',
  // liveness
  alive: 'up', stale: 'amber', dead: 'down', reachable: 'up',
  // sides and liquidity
  buy: 'up', sell: 'down', long: 'up', short: 'down',
  maker: 'up', taker: 'neutral',
  // markets
  perp: 'accent', spot: 'neutral', future: 'violet', option: 'violet',
}

const label: Record<string, string> = {
  partially_filled: 'partial',
  pending_update: 'amending',
  pending_cancel: 'cancelling',
}

export function StatusBadge({ status, className }: { status: string; className?: string }) {
  return (
    <Badge tone={tone[status] ?? 'neutral'} className={className}>
      {label[status] ?? status.replace(/_/g, ' ')}
    </Badge>
  )
}
