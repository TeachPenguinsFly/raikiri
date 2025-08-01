import { cn } from '@/lib/utils'

/** The two glyphs the terminal actually needs.
 *
 *  There were eight. Six of them were decoration standing in for a meaning —
 *  an abstract mark for "Markets", a sun for "Settings", a box for
 *  "Strategies" — and a glyph a reader has to learn is worse than the word it
 *  replaced. The navigation says its words now. What is left is a magnifier,
 *  which is understood everywhere, and a caret, which is a direction. */
type Props = { className?: string; strokeWidth?: number }

function Glyph({ className, strokeWidth = 1.5, children }: Props & { children: React.ReactNode }) {
  return (
    <svg
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={strokeWidth}
      strokeLinecap="round"
      strokeLinejoin="round"
      className={cn('shrink-0', className)}
      aria-hidden="true"
    >
      {children}
    </svg>
  )
}

export const Icon = {
  Search: (p: Props) => (
    <Glyph {...p}>
      <circle cx="7" cy="7" r="4.25" />
      <path d="m10.2 10.2 3 3" />
    </Glyph>
  ),
  Caret: (p: Props) => (
    <Glyph {...p}>
      <path d="m4 6.5 4 3.5 4-3.5" />
    </Glyph>
  ),
}
