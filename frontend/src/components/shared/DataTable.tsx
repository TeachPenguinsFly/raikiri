import { useRef, useState } from 'react'
import {
  flexRender,
  getCoreRowModel,
  getFilteredRowModel,
  getSortedRowModel,
  useReactTable,
  type ColumnDef,
  type SortingState,
} from '@tanstack/react-table'
import { useVirtualizer } from '@tanstack/react-virtual'
import { enterDelay, useEntranceOnce } from '@/lib/entrance'
import { cn } from '@/lib/utils'
import { EmptyState } from './EmptyState'
import { TableSkeleton } from './TableSkeleton'

interface DataTableProps<T> {
  data: T[]
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  columns: ColumnDef<T, any>[]
  initialSorting?: SortingState
  globalFilter?: string
  onRowClick?: (row: T) => void
  isSelected?: (row: T) => boolean
  emptyTitle?: string
  emptyHint?: string
  /** First load still in flight — renders skeleton rows instead of the empty state. */
  loading?: boolean
  dense?: boolean
  className?: string
}

/** An arrow, drawn. Two glyphs did not justify a thousand-icon package. */
function SortArrow({ direction }: { direction: 'asc' | 'desc' }) {
  return (
    <svg viewBox="0 0 24 24" className="size-3" fill="none" stroke="currentColor" strokeWidth={2.5}
         strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      {direction === 'asc' ? <path d="M12 19V5M5 12l7-7 7 7" /> : <path d="M12 5v14M19 12l-7 7-7-7" />}
    </svg>
  )
}

/** Row counts above this render through the virtualizer: a cross-venue board
 *  carries thousands of rows, and mounting them all freezes the tab on every
 *  refresh. */
const VIRTUALIZE_THRESHOLD = 150

export function DataTable<T>({
  data,
  columns,
  initialSorting = [],
  globalFilter,
  onRowClick,
  isSelected,
  emptyTitle = 'No records',
  emptyHint,
  loading,
  dense,
  className,
}: DataTableProps<T>) {
  const [sorting, setSorting] = useState<SortingState>(initialSorting)
  const parentRef = useRef<HTMLDivElement>(null)

  const table = useReactTable({
    data,
    columns,
    state: { sorting, globalFilter },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
    globalFilterFn: 'includesString',
  })

  const rows = table.getRowModel().rows
  const virtual = rows.length > VIRTUALIZE_THRESHOLD

  const virtualizer = useVirtualizer({
    count: rows.length,
    getScrollElement: () => parentRef.current,
    estimateSize: () => (dense ? 25 : 33),
    overscan: 25,
    enabled: virtual,
  })

  const items = virtualizer.getVirtualItems()
  const first = items[0]
  const last = items[items.length - 1]
  const padTop = virtual && first !== undefined ? first.start : 0
  const padBottom =
    virtual && last !== undefined ? virtualizer.getTotalSize() - last.end : 0
  // `.filter(Boolean)` is not decoration: a virtual item can point past the end
  // for one frame after the data shrinks under the scroller, and rendering that
  // hole throws inside the row rather than showing a short table.
  const visibleRows = virtual
    ? items.map((v) => rows[v.index]).filter((row) => row !== undefined)
    : rows

  // Staggered entrance only on the initial data arrival (skeleton→content swap);
  // later poll refreshes and streamed rows render instantly.
  const entering = useEntranceOnce(rows.length > 0)

  return (
    <div ref={parentRef} className={cn('relative w-full overflow-auto', className)}>
      <table className="w-full border-collapse text-[12px]">
        <thead className="sticky top-0 z-10 bg-panel">
          {table.getHeaderGroups().map((hg) => (
            <tr key={hg.id} className="border-b border-line">
              {hg.headers.map((header) => {
                const sorted = header.column.getIsSorted()
                const canSort = header.column.getCanSort()
                return (
                  <th
                    key={header.id}
                    onClick={canSort ? header.column.getToggleSortingHandler() : undefined}
                    className={cn(
                      'h-7 whitespace-nowrap px-2 text-left align-middle text-3xs font-medium uppercase tracking-[0.08em] text-fg-dim',
                      canSort && 'cursor-pointer select-none hover:text-fg-mid transition-colors',
                      (header.column.columnDef.meta as { align?: string } | undefined)?.align === 'right' && 'text-right',
                    )}
                  >
                    <span className="inline-flex items-center gap-0.5">
                      {flexRender(header.column.columnDef.header, header.getContext())}
                      {sorted === 'asc' && <SortArrow direction="asc" />}
                      {sorted === 'desc' && <SortArrow direction="desc" />}
                    </span>
                  </th>
                )
              })}
            </tr>
          ))}
        </thead>
        <tbody>
          {padTop > 0 && <tr style={{ height: padTop }} aria-hidden />}
          {visibleRows.map((row, i) => (
            <tr
              key={row.id}
              data-index={row.id}
              onClick={onRowClick ? () => onRowClick(row.original) : undefined}
              style={entering ? enterDelay(i) : undefined}
              className={cn(
                'border-b border-line/50 transition-colors duration-75',
                entering && 'enter-fade-up',
                onRowClick && 'cursor-pointer',
                isSelected?.(row.original) ? 'bg-accent/8' : 'hover:bg-hover',
              )}
            >
              {row.getVisibleCells().map((cell) => (
                <td
                  key={cell.id}
                  className={cn(
                    'whitespace-nowrap px-2 align-middle',
                    dense ? 'py-[3px]' : 'py-1.5',
                    (cell.column.columnDef.meta as { align?: string } | undefined)?.align === 'right' && 'text-right',
                  )}
                >
                  {flexRender(cell.column.columnDef.cell, cell.getContext())}
                </td>
              ))}
            </tr>
          ))}
          {padBottom > 0 && <tr style={{ height: padBottom }} aria-hidden />}
        </tbody>
      </table>
      {rows.length === 0 && (loading ? <TableSkeleton rows={6} /> : <EmptyState title={emptyTitle} hint={emptyHint} />)}
    </div>
  )
}
