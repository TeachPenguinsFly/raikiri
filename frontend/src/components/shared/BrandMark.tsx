import { cn } from '@/lib/utils'

/** TPF LABS, set with weight.
 *
 *  The first attempt was light and letter-spaced, which reads as a fashion
 *  label rather than a trading desk: on a dark, dense screen a thin wordmark
 *  disappears into the chrome. Institutional software sets its name solid —
 *  the name is the one thing on screen that never needs to be scanned. */
export function BrandMark({ className }: { className?: string }) {
  return (
    <span className={cn('inline-flex items-baseline gap-[3px] leading-none', className)}>
      <span className="font-extrabold tracking-[-0.02em] text-fg">TPF</span>
      <span className="mb-[2px] size-[6px] shrink-0 self-end bg-brand" />
      <span className="font-medium tracking-[-0.01em] text-fg-mid">LABS</span>
    </span>
  )
}

/** Stacked, for the sign-in screen.
 *
 *  The square carries the mark on its own here rather than a letter inside a
 *  tile: a "T" in a rounded box is the shape every SaaS login wears, and the
 *  wordmark underneath already says the name. */
export function BrandLockup({ className }: { className?: string }) {
  return (
    <div className={cn('flex flex-col items-center gap-3.5', className)}>
      <span className="relative flex size-11 items-center justify-center">
        <span className="absolute inset-0 rounded-xl bg-brand/15 blur-md" />
        <span className="relative size-4 rotate-45 bg-brand shadow-[0_0_24px_rgba(56,189,248,0.55)]" />
      </span>
      <BrandMark className="text-xl" />
      <span className="h-px w-40 bg-brand/25" />
    </div>
  )
}
