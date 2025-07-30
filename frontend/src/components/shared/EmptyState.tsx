import type { ReactNode } from 'react'
import { Icon } from './Icon'

export function EmptyState({
  icon,
  title,
  hint,
}: {
  icon?: ReactNode | undefined
  title: string
  hint?: string | undefined
}) {
  return (
    <div className="flex flex-col items-center justify-center gap-2 px-4 py-12 text-center">
      {icon ?? <Icon.Inbox className="size-5 text-fg-dim" strokeWidth={1.5} />}
      <div className="text-xs font-medium text-fg-mid">{title}</div>
      {hint !== undefined && <div className="max-w-[240px] text-2xs text-fg-dim">{hint}</div>}
    </div>
  )
}
