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
        'flex h-8 shrink-0 items-center justify-between border-b border-line px-2.5',
        className,
      )}
    >
      <div className="panel-label flex items-center gap-2">{title}</div>
      {right !== undefined && <div className="flex items-center gap-1.5">{right}</div>}
    </div>
  )
}
