import { Skeleton } from './Skeleton'

/** Placeholder rows while a table's first load is in flight. They fade toward
 *  the bottom so what shows is a soft column of shimmer, not a wall of it. */
export function TableSkeleton({ rows = 8 }: { rows?: number }) {
  return (
    <div aria-hidden className="flex flex-col gap-2.5 p-3">
      {Array.from({ length: rows }).map((_, i) => (
        <div
          key={i}
          className="flex items-center gap-3"
          style={{ opacity: Math.max(1 - i * 0.11, 0.15) }}
        >
          <Skeleton className="h-3 w-16" />
          <Skeleton className="h-3 w-24" />
          <Skeleton className="h-3 flex-1 opacity-50" />
          <Skeleton className="h-3 w-20" />
          <Skeleton className="h-3 w-12" />
        </div>
      ))}
    </div>
  )
}
