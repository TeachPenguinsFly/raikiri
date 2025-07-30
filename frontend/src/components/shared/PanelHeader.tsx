import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'

/** The head of a panel.
 *
 *  No rule underneath it. The title is separated from the content by its own
 *  weight and by 12px of air, which is what separates a heading from a
 *  paragraph in every well-set document; a hairline there is one more line on
 *  a screen that already has a table's worth. */
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
        'flex h-11 shrink-0 items-center justify-between gap-3 px-3',
        className,
      )}
    >
      <div className="panel-title flex min-w-0 items-center gap-2">{title}</div>
      {right !== undefined && <div className="flex shrink-0 items-center gap-2">{right}</div>}
    </div>
  )
}
