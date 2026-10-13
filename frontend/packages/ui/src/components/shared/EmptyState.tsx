export function EmptyState({ title, hint }: { title: string; hint?: string | undefined }) {
  return (
    <div className="px-3 py-6 text-xs text-fg-dim">
      {title}
      {hint !== undefined && <span className="text-fg-dim/70"> — {hint}</span>}
    </div>
  )
}
