import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/utils'

/** tints the background so the side keeps its colour */
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
    const timer = setTimeout(() => setFlash(null), 600)
    return () => clearTimeout(timer)
  }, [value])

  return (
    <span
      // keyed on the flash so a tick mid-animation restarts it
      key={`${flash}-${value}`}
      className={cn(
        'num -mx-1 inline-block rounded-sm px-1',
        flash === 'up' && 'flash-up',
        flash === 'down' && 'flash-down',
        className,
      )}
    >
      {value === null ? '—' : format(value)}
    </span>
  )
}
