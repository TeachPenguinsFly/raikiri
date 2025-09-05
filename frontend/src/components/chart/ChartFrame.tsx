import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'
import { useMeasuredWidth } from './scale'

export interface LegendEntry {
  key: string
  label: string
  color: string
  /** Already formatted. The frame renders numbers, it does not format them. */
  value?: string | undefined
  muted?: boolean | undefined
}

interface ChartFrameProps {
  title: string
  /** One line under the title. Say what the number IS, especially when it is
   *  not the number a reader would assume. */
  note?: string | undefined
  legend?: LegendEntry[] | undefined
  onLegendHover?: ((key: string | null) => void) | undefined
  /** The headline figure, right-aligned against the title. */
  readout?: ReactNode | undefined
  height?: number | undefined
  /** Shown instead of the plot. A chart with nothing in it must say which
   *  nothing: not loaded, or loaded and empty. */
  placeholder?: string | undefined
  footer?: ReactNode | undefined
  className?: string | undefined
  children: (size: { width: number; height: number }) => ReactNode
}

/** The frame every plot in the terminal draws inside.
 *
 *  It owns the border, the header, the legend, the measured plot box and the
 *  empty state — so a donut and a line chart are the same object at a glance,
 *  and a new plot is a `children` function rather than another copy of this
 *  chrome. Nothing here knows what is being drawn. */
export function ChartFrame({
  title,
  note,
  legend,
  onLegendHover,
  readout,
  height = 240,
  placeholder,
  footer,
  className,
  children,
}: ChartFrameProps) {
  const [ref, width] = useMeasuredWidth<HTMLDivElement>()

  return (
    <section
      className={cn(
        'panel',
        className,
      )}
    >
      <header className="flex items-start justify-between gap-4 px-3 pt-3">
        <div className="min-w-0">
          <h2 className="panel-title">{title}</h2>
          {note !== undefined && <p className="mt-1 text-xs text-fg-dim">{note}</p>}
        </div>
        {readout !== undefined && <div className="shrink-0 text-right">{readout}</div>}
      </header>

      {legend !== undefined && legend.length > 0 && (
        <div
          className="flex flex-wrap items-center gap-x-4 gap-y-1.5 px-3 pt-3"
          onMouseLeave={() => onLegendHover?.(null)}
        >
          {legend.map((entry) => (
            <button
              key={entry.key}
              type="button"
              onMouseEnter={() => onLegendHover?.(entry.key)}
              className={cn(
                'flex items-center gap-1.5 rounded-sm transition-opacity duration-150',
                entry.muted === true ? 'opacity-40' : 'opacity-100',
                onLegendHover !== undefined ? 'cursor-pointer' : 'cursor-default',
              )}
            >
              <span
                className="size-1.5 rounded-full"
                style={{ backgroundColor: entry.color, boxShadow: `0 0 8px ${entry.color}66` }}
              />
              <span className="text-xs text-fg-mid">{entry.label}</span>
              {entry.value !== undefined && (
                <span className="num text-xs font-medium" style={{ color: entry.color }}>
                  {entry.value}
                </span>
              )}
            </button>
          ))}
        </div>
      )}

      <div ref={ref} className="px-2 pt-3 pb-2" style={{ height }}>
        {placeholder !== undefined ? (
          <div className="flex h-full items-center justify-center text-xs text-fg-dim">
            {placeholder}
          </div>
        ) : width > 0 ? (
          children({ width: width - 16, height: height - 12 })
        ) : null}
      </div>

      {footer !== undefined && (
        <div className="px-3 pb-3 text-2xs text-fg-dim">{footer}</div>
      )}
    </section>
  )
}
