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
        'shadow-[0_1px_0_0_rgba(255,255,255,0.03)_inset,0_18px_40px_-24px_rgba(0,0,0,0.9)]',
        className,
      )}
    >
      <header className="flex items-start justify-between gap-4 px-2.5 pt-2">
        <div className="min-w-0">
          <h2 className="panel-label">{title}</h2>
          {note !== undefined && <p className="mt-1 text-2xs text-fg-mid">{note}</p>}
        </div>
        {readout !== undefined && <div className="shrink-0 text-right">{readout}</div>}
      </header>

      {legend !== undefined && legend.length > 0 && (
        <div
          className="flex flex-wrap items-center gap-x-4 gap-y-1.5 px-2.5 pt-2.5"
          onMouseLeave={() => onLegendHover?.(null)}
        >
          {legend.map((entry) => (
            <button
              key={entry.key}
              type="button"
              onMouseEnter={() => onLegendHover?.(entry.key)}
              className={cn(
                'flex items-center gap-1.5 rounded transition-opacity',
                entry.muted === true ? 'opacity-40' : 'opacity-100',
                onLegendHover !== undefined ? 'cursor-pointer' : 'cursor-default',
              )}
            >
              <span
                className="size-1.5 rounded-full"
                style={{ backgroundColor: entry.color, boxShadow: `0 0 8px ${entry.color}66` }}
              />
              <span className="text-2xs font-medium text-fg-mid">{entry.label}</span>
              {entry.value !== undefined && (
                <span className="num text-2xs font-semibold" style={{ color: entry.color }}>
                  {entry.value}
                </span>
              )}
            </button>
          ))}
        </div>
      )}

      <div ref={ref} className="px-2 pt-2 pb-1" style={{ height }}>
        {placeholder !== undefined ? (
          <div className="flex h-full items-center justify-center text-xs text-fg-dim">
            {placeholder}
          </div>
        ) : width > 0 ? (
          children({ width: width - 16, height: height - 12 })
        ) : null}
      </div>

      {footer !== undefined && (
        <div className="border-t border-line/60 px-2.5 py-2 text-3xs text-fg-mid">{footer}</div>
      )}
    </section>
  )
}
