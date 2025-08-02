import { cn } from '@/lib/utils'

/** TPF·LABS.
 *
 *  The original wordmark, restored: solid weight, a hair of letter-spacing,
 *  and the brand cyan square between the two words. The versions in between —
 *  a light letter-spaced setting, then a colour break with no separator —
 *  were both attempts to make it quieter, and both cost it its identity. A
 *  wordmark is the one thing on a screen that is allowed to be itself. */
export function BrandMark({ className }: { className?: string }) {
  return (
    <span
      className={cn(
        'inline-flex items-center font-extrabold tracking-[0.03em] text-fg',
        className,
      )}
    >
      TPF
      <span className="mx-[5px] inline-block size-[9px] rounded-[1px] bg-accent" />
      LABS
    </span>
  )
}

/** Stacked, for the sign-in screen. */
export function BrandLockup({ className }: { className?: string }) {
  return (
    <div className={cn('flex flex-col items-center', className)}>
      <BrandMark className="text-xl" />
    </div>
  )
}
