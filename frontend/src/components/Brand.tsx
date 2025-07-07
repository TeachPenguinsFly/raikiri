import { cn } from '@/lib/format'

/** TPF·LABS, drawn rather than loaded.
 *
 *  The brand is a wordmark and a square, so it is type and a rectangle — an
 *  image file would be one more request, one more thing to go stale against
 *  the brand directory, and blurry at the sizes a terminal chrome uses. */

/** The compact mark: the square with the T, for a header or a tab. */
export function Mark({ className }: { className?: string | undefined }) {
  return (
    <svg viewBox="0 0 32 32" className={cn('shrink-0', className)} aria-hidden="true">
      <rect width="32" height="32" rx="7" fill="#38bdf8" />
      <text
        x="16"
        y="17"
        textAnchor="middle"
        dominantBaseline="central"
        fontFamily="Inter, Helvetica, Arial, sans-serif"
        fontWeight="800"
        fontSize="18"
        fill="#030508"
      >
        T
      </text>
    </svg>
  )
}

/** The horizontal wordmark. `product` sets the platform name beside it. */
export function Wordmark({
  className,
  product,
}: {
  className?: string | undefined
  product?: string | undefined
}) {
  return (
    <span className={cn('flex items-center', className)}>
      <span className="flex items-center font-light tracking-[0.14em] text-ink">
        TPF
        <span className="mx-1.5 inline-block size-[7px] bg-brand" />
        LABS
      </span>
      {product !== undefined && (
        <>
          <span className="mx-2.5 h-3 w-px bg-line" />
          <span className="font-mono tracking-tight text-muted">{product}</span>
        </>
      )}
    </span>
  )
}

/** Stacked, for a sign-in screen: mark above, wordmark under it. */
export function BrandLockup({ className }: { className?: string | undefined }) {
  return (
    <div className={cn('flex flex-col items-center gap-4', className)}>
      <Mark className="size-12 drop-shadow-[0_8px_24px_rgba(56,189,248,0.3)]" />
      <span className="flex items-center text-lg font-light tracking-[0.18em] text-ink">
        TPF
        <span className="mx-2 inline-block size-2 bg-brand" />
        LABS
      </span>
      <span className="h-px w-40 bg-brand/25" />
    </div>
  )
}
