import { Badge, type BadgeTone } from './Badge'

/** One vocabulary for every state the terminal shows, so `filled` is the same
 *  green on an order blotter and on a fill tape. A status with no entry reads
 *  neutral rather than inventing a colour for a word nobody mapped.
 *
 *  Most of these are NEUTRAL on purpose. `accepted` is not good news and
 *  `cancelling` is not bad news — they are just where the order is, and only
 *  the states that carry direction or trouble earn a colour. */
const tone: Record<string, BadgeTone> = {
  // orders — colour only where the state is a problem or a completion
  initialized: 'neutral', submitted: 'neutral', accepted: 'neutral',
  partially_filled: 'amber', filled: 'up', canceled: 'neutral',
  rejected: 'down', denied: 'down', expired: 'neutral', triggered: 'amber',
  pending_update: 'amber', pending_cancel: 'amber',
  // reconciliation
  agree: 'up', break: 'down', unknown: 'amber',
  // liveness — `alive` is the expected state and reads neutral. Eight green
  // pills in a column of eight is a column nobody scans, and the one row that
  // matters is the one that is not green.
  alive: 'neutral', stale: 'amber', dead: 'down', reachable: 'neutral',
  // sides and liquidity
  buy: 'up', sell: 'down', long: 'up', short: 'down',
  maker: 'neutral', taker: 'neutral',
  // markets
  perp: 'neutral', spot: 'neutral', future: 'violet', option: 'violet',
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
