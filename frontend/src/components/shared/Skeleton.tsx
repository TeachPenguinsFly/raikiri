import type { CSSProperties } from 'react'
import { cn } from '@/lib/utils'

/** Base shimmer bar (see `.skeleton` in index.css). Size it with classes. */
export function Skeleton({ className, style }: { className?: string | undefined; style?: CSSProperties | undefined }) {
  return <div aria-hidden className={cn('skeleton', className)} style={style} />
}

/** Right-aligned numeric placeholder for a table cell. */
export function SkeletonNum({ className }: { className?: string }) {
  return <Skeleton className={cn('ml-auto h-2.5 w-12', className)} />
}
