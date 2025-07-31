import { cn } from '@/lib/utils'

/** TPF LABS.
 *
 *  Set as one word in two weights rather than two colours: at 14px in a 200px
 *  rail the colour break was the loudest thing in the navigation, and a logo
 *  that outshouts the data under it is a logo in the wrong place. The accent
 *  is spent on the mark instead, which is four pixels wide. */
export function BrandMark({ className }: { className?: string }) {
  return (
    <span className={cn('inline-flex items-center gap-2 leading-none', className)}>
      <span className="size-1.5 shrink-0 rotate-45 bg-accent" />
      <span className="tracking-[-0.015em]">
        <span className="font-semibold text-fg">TPF</span>
        <span className="font-medium text-fg-mid"> LABS</span>
      </span>
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
