import { useEffect, useRef, useState } from 'react'
import { cn } from '@/lib/utils'

/** A price that marks its own change.
 *
 *  The tint is on the BACKGROUND, not on the figure. Recolouring the number
 *  meant a bid turned red on a downtick — which is the ask's colour, on the
 *  column whose whole job is to be the bid. The side keeps its colour and the
 *  change gets a wash behind it, which is also the quieter of the two: a
 *  board of twenty venues repainting its digits twice a second is a board you
 *  stop reading. */
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
      // Keyed on the flash so the animation restarts on a tick that arrives
      // while the last one is still running.
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
