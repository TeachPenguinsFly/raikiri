import { cn } from '@/lib/utils'

/** The seven icons the shell actually uses, drawn rather than installed.
 *
 *  `lucide-react` was reached for in eighteen files to draw about a dozen
 *  glyphs; the package is a thousand icons and a build-time dependency for
 *  every one of them. These are the same shapes at the same stroke weight. */
type Props = { className?: string; strokeWidth?: number }

function Glyph({ className, strokeWidth = 2, children }: Props & { children: React.ReactNode }) {
  return (
    <svg
      viewBox="0 0 24 24"
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
  Markets: (p: Props) => (
    <Glyph {...p}>
      <path d="M9 5v4M9 15v4M15 5v2M15 13v6" />
      <rect x="6" y="9" width="6" height="6" rx="1" />
      <rect x="12" y="7" width="6" height="6" rx="1" />
    </Glyph>
  ),
  Radar: (p: Props) => (
    <Glyph {...p}>
      <circle cx="12" cy="12" r="9" />
      <circle cx="12" cy="12" r="4.5" />
      <path d="M12 12 19 7" />
    </Glyph>
  ),
  Target: (p: Props) => (
    <Glyph {...p}>
      <circle cx="12" cy="12" r="8" />
      <circle cx="12" cy="12" r="3.5" />
      <path d="M12 4v2M12 18v2M4 12h2M18 12h2" />
    </Glyph>
  ),
  Bot: (p: Props) => (
    <Glyph {...p}>
      <rect x="4" y="8" width="16" height="11" rx="3" />
      <path d="M12 5v3M8.5 13h.01M15.5 13h.01" />
    </Glyph>
  ),
  Landmark: (p: Props) => (
    <Glyph {...p}>
      <path d="M3 20h18M5 20v-8M9.5 20v-8M14.5 20v-8M19 20v-8M3 9l9-5 9 5H3z" />
    </Glyph>
  ),
  Settings: (p: Props) => (
    <Glyph {...p}>
      <circle cx="12" cy="12" r="3" />
      <path d="M12 3v2.2M12 18.8V21M21 12h-2.2M5.2 12H3M18.4 5.6l-1.6 1.6M7.2 16.8l-1.6 1.6M18.4 18.4l-1.6-1.6M7.2 7.2 5.6 5.6" />
    </Glyph>
  ),
  Inbox: (p: Props) => (
    <Glyph {...p}>
      <path d="M4 13h4l1.5 3h5L16 13h4" />
      <path d="M5.5 5h13l2.5 8v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-5l2.5-8z" />
    </Glyph>
  ),
  Search: (p: Props) => (
    <Glyph {...p}>
      <circle cx="11" cy="11" r="6.5" />
      <path d="m16 16 4 4" />
    </Glyph>
  ),
}
