import { cn } from '@/lib/utils'

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
