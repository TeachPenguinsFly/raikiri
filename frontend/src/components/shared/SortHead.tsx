import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'

export interface Sort<K extends string> {
  key: K
  desc: boolean
}

/** A column header that sorts.
 *
 *  The caret shows only on the active column — a caret beside every header is
 *  a row of decoration, and the reader stops seeing the one that matters. */
export function SortHead<K extends string>({
  k,
  sort,
  onSort,
  right,
  children,
}: {
  k: K
  sort: Sort<K>
  onSort: (s: Sort<K>) => void
  right?: boolean
  children: ReactNode
}) {
  const active = sort.key === k
  return (
    <th className={cn('col-head px-3', right === true ? 'text-right' : 'text-left')}>
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
