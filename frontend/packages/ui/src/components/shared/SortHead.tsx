import type { ReactNode } from 'react'
import { cn } from '../../lib/format'

export interface Sort<K extends string> {
  key: K
  desc: boolean
}

export function SortHead<K extends string>({
  k,
  sort,
  onSort,
  right,
  className,
  children,
}: {
  k: K
  sort: Sort<K>
  onSort: (s: Sort<K>) => void
  right?: boolean
  /** sticky headers need an opaque background on the cells */
  className?: string | undefined
  children: ReactNode
}) {
  const active = sort.key === k
  return (
    <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left', className)}>
      <button
        type="button"
        onClick={() => onSort({ key: k, desc: active ? !sort.desc : true })}
        className={cn(
          'inline-flex items-center gap-1 transition-colors duration-150 hover:text-fg-mid',
          active && 'text-fg-mid',
        )}
      >
        {children}
        {active && <span className="text-[8px] leading-none">{sort.desc ? '▼' : '▲'}</span>}
      </button>
    </th>
  )
}
