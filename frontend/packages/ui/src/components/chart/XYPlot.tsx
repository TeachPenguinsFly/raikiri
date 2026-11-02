import { useMemo, useState } from 'react'
import { linear, niceTicks } from './scale'

export interface XYPoint {
  x: number
  y: number
}

export interface XYSeries {
  key: string
  label: string
  color: string
  points: XYPoint[]
  /** dots for quotes, a line for a fit or a term structure */
  mode: 'line' | 'dots' | 'both'
}

const k_pad = { top: 12, right: 52, bottom: 22, left: 8 }

function padded(values: number[]): [number, number] {
  if (values.length === 0) return [0, 1]
  const min = Math.min(...values)
  const max = Math.max(...values)
  if (min === max) {
    const nudge = Math.abs(min) * 0.05 || 1
    return [min - nudge, max + nudge]
  }
  const padding = (max - min) * 0.08
  return [min - padding, max + padding]
}

/** a numeric x axis, unlike LinePlot's time axis: strikes, days to expiry */
export function XYPlot({
  series,
  width,
  height,
  formatX,
  formatY,
  markers = [],
  highlight,
}: {
  series: XYSeries[]
  width: number
  height: number
  formatX: (x: number) => string
  formatY: (y: number) => string
  /** vertical guides, e.g. the forward */
  markers?: Array<{ x: number; label: string }>
  highlight?: string | null | undefined
}) {
  const [hover, setHover] = useState<{ key: string; point: XYPoint } | null>(null)

  const geometry = useMemo(() => {
    const all = series.flatMap((s) => s.points)
    if (all.length === 0 || width < 40) return null
    const plotWidth = width - k_pad.left - k_pad.right
    const plotHeight = height - k_pad.top - k_pad.bottom
    const [minX, maxX] = padded([...all.map((p) => p.x), ...markers.map((m) => m.x)])
    const [minY, maxY] = padded(all.map((p) => p.y))
    const px = linear([minX, maxX], [k_pad.left, k_pad.left + plotWidth])
    const py = linear([minY, maxY], [k_pad.top + plotHeight, k_pad.top])
    return {
      px,
      py,
      yTicks: niceTicks(minY, maxY, 4).map((v) => ({ v, y: py(v) })),
      xTicks: niceTicks(minX, maxX, Math.max(2, Math.min(8, Math.floor(plotWidth / 60)))).map(
        (v) => ({
          v,
          x: px(v),
        }),
      ),
      lines: series.map((s) => ({
        ...s,
        path: [...s.points]
          .sort((a, b) => a.x - b.x)
          .map((p, i) => `${i === 0 ? 'M' : 'L'}${px(p.x).toFixed(1)},${py(p.y).toFixed(1)}`)
          .join(' '),
      })),
    }
  }, [series, width, height, markers])

  if (geometry === null) return null

  const onMove = (event: React.MouseEvent<SVGSVGElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect()
    const mx = event.clientX - bounds.left
    const my = event.clientY - bounds.top
    let best: { key: string; point: XYPoint } | null = null
    let distance = 24 * 24
    for (const s of series) {
      for (const point of s.points) {
        const d = (geometry.px(point.x) - mx) ** 2 + (geometry.py(point.y) - my) ** 2
        if (d < distance) {
          distance = d
          best = { key: s.key, point }
        }
      }
    }
    setHover(best)
  }

  const hovered = hover === null ? undefined : series.find((s) => s.key === hover.key)

  return (
    <div className="relative">
      <svg
        width={width}
        height={height}
        className="block overflow-visible"
        onMouseMove={onMove}
        onMouseLeave={() => setHover(null)}
      >
        {geometry.yTicks.map((tick) => (
          <g key={`y${tick.v}`}>
            <line
              x1={k_pad.left}
              y1={tick.y.toFixed(1)}
              x2={width - k_pad.right}
              y2={tick.y.toFixed(1)}
              stroke="#ffffff"
              strokeOpacity={0.045}
            />
            <text
              x={width - k_pad.right + 8}
              y={tick.y + 3.5}
              fontSize={11}
              fill="#8b97ad"
              className="tnum"
            >
              {formatY(tick.v)}
            </text>
          </g>
        ))}
        {geometry.xTicks.map((tick) => (
          <text
            key={`x${tick.v}`}
            x={tick.x.toFixed(1)}
            y={height - 6}
            fontSize={11}
            fill="#5b6780"
            textAnchor="middle"
            className="tnum"
          >
            {formatX(tick.v)}
          </text>
        ))}
        {markers.map((m) => (
          <g key={`m${m.label}`}>
            <line
              x1={geometry.px(m.x).toFixed(1)}
              y1={k_pad.top}
              x2={geometry.px(m.x).toFixed(1)}
              y2={height - k_pad.bottom}
              stroke="#8b97ad"
              strokeOpacity={0.35}
              strokeDasharray="2 3"
            />
            <text x={geometry.px(m.x) + 4} y={k_pad.top + 8} fontSize={10} fill="#8b97ad">
              {m.label}
            </text>
          </g>
        ))}
        {geometry.lines.map((s) => {
          const dim = highlight !== null && highlight !== undefined && highlight !== s.key
          return (
            <g key={s.key} opacity={dim ? 0.25 : 1} className="transition-opacity duration-200">
              {s.mode !== 'dots' && (
                <path
                  d={s.path}
                  fill="none"
                  stroke={s.color}
                  strokeWidth={1.6}
                  strokeLinejoin="round"
                  strokeLinecap="round"
                />
              )}
              {s.mode !== 'line' &&
                s.points.map((p) => (
                  <circle
                    key={`${p.x}:${p.y}`}
                    cx={geometry.px(p.x).toFixed(1)}
                    cy={geometry.py(p.y).toFixed(1)}
                    r={2.4}
                    fill={s.color}
                    fillOpacity={0.85}
                  />
                ))}
            </g>
          )
        })}
        {hover !== null && (
          <circle
            cx={geometry.px(hover.point.x)}
            cy={geometry.py(hover.point.y)}
            r={4}
            fill="none"
            stroke="#e6ebf5"
            strokeWidth={1.2}
          />
        )}
      </svg>
      {hover !== null && hovered !== undefined && (
        <div
          className="pointer-events-none absolute top-0 z-10 rounded-md bg-raised px-2.5 py-1.5 text-xs shadow-[0_12px_32px_-8px_rgba(0,0,0,0.7)]"
          style={{
            left: Math.min(Math.max(geometry.px(hover.point.x) - 60, 0), Math.max(width - 140, 0)),
          }}
        >
          <div className="flex items-center gap-1.5">
            <span className="size-1.5 rounded-full" style={{ backgroundColor: hovered.color }} />
            <span className="text-fg-mid">{hovered.label}</span>
          </div>
          <div className="tnum text-fg">
            {formatX(hover.point.x)} · {formatY(hover.point.y)}
          </div>
        </div>
      )}
    </div>
  )
}
