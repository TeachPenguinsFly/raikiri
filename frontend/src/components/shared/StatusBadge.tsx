/** colour only for direction or trouble */
const k_tone: Record<string, string> = {
  buy: 'text-up',
  sell: 'text-down',
  filled: 'text-up',
  rejected: 'text-down',
  denied: 'text-down',
  partially_filled: 'text-amber',
  triggered: 'text-amber',
  pending_update: 'text-amber',
  pending_cancel: 'text-amber',
  stale: 'text-amber',
}

const k_label: Record<string, string> = {
  partially_filled: 'Partial',
  pending_update: 'Amending',
  pending_cancel: 'Cancelling',
}

const title = (s: string) => s.charAt(0).toUpperCase() + s.slice(1)

export function StatusBadge({ status }: { status: string }) {
  return (
    <span className={k_tone[status] ?? 'text-fg-mid'}>
      {k_label[status] ?? title(status.replace(/_/g, ' '))}
    </span>
  )
}
