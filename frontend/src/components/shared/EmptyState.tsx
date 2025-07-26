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
    <div className="flex flex-col items-center justify-center gap-1.5 py-10 text-center">
      {icon ?? <Icon.Inbox className="size-5 text-fg-dim" strokeWidth={1.5} />}
      <div className="text-2xs font-medium text-fg-mid">{title}</div>
      {hint !== undefined && <div className="max-w-[260px] text-3xs text-fg-dim">{hint}</div>}
    </div>
  )
}
