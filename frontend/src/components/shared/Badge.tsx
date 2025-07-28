import type { HTMLAttributes } from 'react'
import { cn } from '@/lib/utils'

/** Seven looks, one lookup.
 *
 *  `class-variance-authority` was a dependency to turn a variant name into a
 *  class string; that is a record. Keeping it a record also keeps the variants
 *  where a reader can see all of them at once. */
export type BadgeTone = 'neutral' | 'outline' | 'up' | 'down' | 'amber' | 'accent' | 'violet'

const tones: Record<BadgeTone, string> = {
  neutral: 'bg-white/7 text-fg-mid',
  outline: 'border border-line2 bg-transparent text-fg-dim',
  up: 'bg-up/12 text-up',
  down: 'bg-down/12 text-down',
  amber: 'bg-amber/12 text-amber',
  accent: 'bg-accent/15 text-accent',
  violet: 'bg-violet/12 text-violet',
}

export function Badge({
  tone = 'neutral',
  className,
  ...rest
}: HTMLAttributes<HTMLSpanElement> & { tone?: BadgeTone }) {
  return (
    <span
      className={cn(
        'inline-flex items-center gap-1 rounded-[3px] px-1.5 py-px text-2xs leading-4 font-semibold whitespace-nowrap',
        tones[tone],
        className,
      )}
      {...rest}
    />
  )
}
