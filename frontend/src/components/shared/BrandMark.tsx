import { cn } from '@/lib/utils'

/** TPF·LABS wordmark, drawn from tpf-website/brand: light weight, letter-spaced,
 *  cyan square separator. Type and a rectangle, so it stays crisp at any size
 *  and there is no asset to go stale against the brand directory. */
export function BrandMark({ className }: { className?: string }) {
  return (
    <span className={cn('inline-flex items-center font-light tracking-[0.14em] text-fg', className)}>
      TPF
      <span className="mx-1.5 inline-block size-[7px] bg-brand" />
      LABS
    </span>
  )
}

/** The compact mark: the square with the T, for a tab or a tight header. */
export function Mark({ className }: { className?: string }) {
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
