export function Silent({ title }: { title?: string }) {
  return (
    <span className="text-fg-dim" title={title}>
      —
    </span>
  )
}
