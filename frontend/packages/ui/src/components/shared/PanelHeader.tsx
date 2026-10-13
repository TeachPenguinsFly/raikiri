import type { ReactNode } from 'react'

export function PanelHeader({ title, right }: { title: ReactNode; right?: ReactNode | undefined }) {
  return (
    <div className="flex h-7 shrink-0 items-center justify-between gap-3 px-3">
      <div className="panel-title flex min-w-0 items-center gap-2">{title}</div>
      {right !== undefined && <div className="flex shrink-0 items-center gap-2">{right}</div>}
    </div>
  )
}
