import type { ReactNode } from 'react'
import { cn } from '@/lib/format'
import { useMeasuredWidth } from './scale'

interface LegendEntry {
  key: string
  label: string
  color: string
  value?: string | undefined
  muted?: boolean | undefined
}

interface ChartFrameProps {
  title: string
  note?: string | undefined
  legend?: LegendEntry[] | undefined
  onLegendHover?: ((key: string | null) => void) | undefined
  readout?: ReactNode | undefined
  height?: number | undefined
  placeholder?: string | undefined
  children: (size: { width: number; height: number }) => ReactNode
}

export function ChartFrame({
  title,
  note,
  legend,
  onLegendHover,
  readout,
  height = 240,
  placeholder,
  children,
}: ChartFrameProps) {
  const [ref, width] = useMeasuredWidth<HTMLDivElement>()

  return (
    <section className="panel">
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
    </section>
  )
}
