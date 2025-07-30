import type { HTMLAttributes } from 'react'
import { cn } from '@/lib/utils'

/** A small state chip.
 *
 *  Tinted text on a barely-there wash rather than a filled pill: a blotter
 *  where every row carries a saturated capsule reads as a status board, and
 *  the status is the least important column in it. */
export type BadgeTone = 'neutral' | 'up' | 'down' | 'amber' | 'accent' | 'violet'

const tones: Record<BadgeTone, string> = {
  neutral: 'bg-white/[0.05] text-fg-mid',
  up: 'bg-up/[0.12] text-up',
  down: 'bg-down/[0.12] text-down',
  amber: 'bg-amber/[0.12] text-amber',
  accent: 'bg-accent/[0.12] text-accent',
  violet: 'bg-violet/[0.12] text-violet',
}

export function Badge({
  tone = 'neutral',
  className,
  ...rest
}: HTMLAttributes<HTMLSpanElement> & { tone?: BadgeTone }) {
  return (
    <span
      className={cn(
        'inline-flex h-[18px] items-center rounded-sm px-1.5 text-2xs font-medium whitespace-nowrap',
        tones[tone],
        className,
      )}
      {...rest}
    />
  )
}
