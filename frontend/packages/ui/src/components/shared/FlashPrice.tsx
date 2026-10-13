import { useEffect, useRef, useState } from 'react'
import { cn, price } from '../../lib/format'

const k_flash_ms = 600

export function FlashPrice({
  value,
  places,
  className,
}: {
  value: string
  places: number
  className?: string
}) {
  const previous = useRef(value)
  const [flash, setFlash] = useState<'up' | 'down' | null>(null)

  useEffect(() => {
    const before = Number(previous.current)
    const now = Number(value)
    previous.current = value
    if (now === before) return
    setFlash(now > before ? 'up' : 'down')
    const timer = setTimeout(() => setFlash(null), k_flash_ms)
    return () => clearTimeout(timer)
  }, [value])

  return (
    <span
      // a new key restarts the animation when a tick lands mid-flash
      key={`${flash}-${value}`}
      className={cn(
        'num -mx-1 inline-block rounded-sm px-1',
        flash === 'up' && 'flash-up',
        flash === 'down' && 'flash-down',
        className,
      )}
    >
      {price(value, places)}
    </span>
  )
}
