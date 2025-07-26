import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/utils'

/** A number that briefly tints on change.
 *
 *  The tint is the point: on a board of twenty venues, which row MOVED is the
 *  question, and a static column of prices answers it only if you were already
 *  looking at that row. */
export function FlashPrice({
  value,
  format = (v) => String(v),
  className,
}: {
  value: number | null
  format?: (v: number) => string
  className?: string
}) {
  const previous = useRef(value)
  const [flash, setFlash] = useState<'up' | 'down' | null>(null)

  useEffect(() => {
    if (value === null) return
    if (previous.current !== null && value > previous.current) setFlash('up')
    else if (previous.current !== null && value < previous.current) setFlash('down')
    previous.current = value
    const timer = setTimeout(() => setFlash(null), 700)
    return () => clearTimeout(timer)
  }, [value])

  return (
    <span
      className={cn(
        'num transition-colors duration-300',
        flash === 'up' && 'text-up',
        flash === 'down' && 'text-down',
        className,
      )}
    >
      {value === null ? '—' : format(value)}
    </span>
  )
}
