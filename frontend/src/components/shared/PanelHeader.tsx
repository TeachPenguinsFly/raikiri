import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'

/** The head of a panel.
 *
 *  A 28px strip carrying a 12px label, and no rule underneath it. A panel head
 *  set at a web heading's size spends forty-four pixels of a trading screen
 *  saying a word the reader already knows; the separation comes from the
 *  label's weight, not from a hairline on a screen that already has a table's
 *  worth. */
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
