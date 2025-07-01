import { cn } from '@/lib/format'

/** The mark: a bolt cut out of the tile. 雷切 is "lightning cutter", and the
 *  glyph is carried beside the mark rather than drawn into it, so the wordmark
 *  survives at 16 px where the kanji would not. */
export function Mark({ className }: { className?: string | undefined }) {
  return (
    <svg viewBox="0 0 32 32" className={cn('shrink-0', className)} aria-hidden="true">
      <defs>
        <linearGradient id="mark-fill" x1="0" y1="0" x2="1" y2="1">
          <stop stopColor="#60a5fa" />
          <stop offset="1" stopColor="#1d4ed8" />
        </linearGradient>
      </defs>
      <rect width="32" height="32" rx="9" fill="url(#mark-fill)" />
      <path d="M18.9 4.6 10.6 17.6h4.9L13.1 27.4l8.3-13h-4.9z" fill="#060b16" />
    </svg>
  )
}

export function Wordmark({ className }: { className?: string | undefined }) {
  return (
    <span className={cn('flex items-center gap-2', className)}>
      <Mark className="size-5" />
      <span className="font-mono tracking-tight text-ink">raikiri</span>
      <span className="text-accent">雷切</span>
    </span>
  )
}
