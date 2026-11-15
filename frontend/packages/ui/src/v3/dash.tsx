import type { CSSProperties, ReactNode } from 'react'
import { Link } from 'react-router'
import { Glyph, Ticker } from './ui'

const money = (v: number) => `$${v.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })}`

/** one figure of a dashboard's top strip, running to each new value */
export function Kpi({
  label,
  value,
  format = money,
  sub,
  d,
  to,
}: {
  label: ReactNode
  value: number | undefined
  format?: (v: number) => string
  sub?: ReactNode
  d: number
  to: string
}) {
  return (
    <Link
      to={to}
      className="v3-rise flex min-w-0 flex-col gap-1 px-4 py-3 transition-colors hover:bg-raised/40"
      style={{ '--d': `${d}ms` } as CSSProperties}
    >
      <span className="flex items-center gap-1.5 text-[10px] font-medium tracking-wider text-fg-dim uppercase">
        {label}
      </span>
      <Ticker value={value} format={format} className="mono text-lg font-semibold text-fg" />
      {sub !== undefined && <span className="text-2xs text-fg-dim">{sub}</span>}
    </Link>
  )
}

export function Heading({
  glyph,
  title,
  to,
  more = 'See all',
  children,
}: {
  glyph?: ReactNode
  title: string
  to?: string
  more?: string
  children?: ReactNode
}) {
  return (
    <div className="flex items-center gap-2">
      {glyph}
      <h2 className="text-sm font-semibold text-fg">{title}</h2>
      {children}
      {to !== undefined && (
        <Link
          to={to}
          className="ml-auto flex items-center gap-1 text-xs text-fg-dim transition-colors hover:text-accent"
        >
          {more}
          <Glyph name="arrow" className="size-3" />
        </Link>
      )}
    </div>
  )
}
