import { cn } from '@/lib/utils'

/** TPF LABS, set as one word in two colours.
 *
 *  Earlier versions put a small square BETWEEN the two words, which at 15px
 *  reads as a stray glyph rather than as a mark — a logo that looks like a
 *  rendering fault is worse than no logo. The colour break carries the same
 *  separation and cannot be mistaken for an artefact. */
export function BrandMark({ className }: { className?: string }) {
  return (
    <span
      className={cn(
        'inline-flex items-baseline gap-[0.28em] font-extrabold leading-none tracking-[-0.01em]',
        className,
      )}
    >
      <span className="text-fg">TPF</span>
      <span className="text-brand">LABS</span>
    </span>
  )
}

/** Stacked, for the sign-in screen: the wordmark over a rule in the accent. */
export function BrandLockup({ className }: { className?: string }) {
  return (
    <div className={cn('flex flex-col items-center gap-4', className)}>
      <BrandMark className="text-[26px]" />
      <span className="h-px w-32 bg-brand/30" />
    </div>
  )
}
