import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'

export function PanelHeader({
  title,
  right,
  className,
}: {
  title: ReactNode
  right?: ReactNode | undefined
  className?: string | undefined
}) {
  return (
    <div
      className={cn(
        'flex h-7 shrink-0 items-center justify-between gap-3 px-3',
        className,
      )}
    >
      <div className="panel-title flex min-w-0 items-center gap-2">{title}</div>
      {right !== undefined && <div className="flex shrink-0 items-center gap-2">{right}</div>}
    </div>
  )
}
