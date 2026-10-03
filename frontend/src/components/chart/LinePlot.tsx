import { useMemo, useState } from 'react'
import { clockTime, compact, dateTime, shortDate } from '@/lib/format'
import { linear, niceTicks, paddedDomain } from './scale'

interface PlotSeries {
  key: string
  label: string
  color: string
  /** aligned with x by index */
  values: number[]
  /** at most one series should set it */
  area?: boolean | undefined
}

const k_pad = { top: 12, right: 52, bottom: 22, left: 8 }

/** labels follow the tick step so neighbouring ticks never print the same text */
function axisLabel(step: number): (value: number) => string {
  if (step >= 1000) return compact
  const decimals = Math.max(0, Math.min(6, Math.ceil(-Math.log10(step))))
  // adding zero turns -0 into 0
  return (value) =>
    (value === 0 ? 0 : value).toLocaleString('en-US', {
      minimumFractionDigits: decimals,
      maximumFractionDigits: decimals,
    })
}

function labelFor(span: number): (millis: number) => string {
  if (span > 3 * 86_400_000) return shortDate
  if (span > 6 * 3_600_000) return dateTime
  return clockTime
}

function xTicks(
  x: number[],
  count: number,
  px: (value: number) => number,
): Array<{ t: number; x: number }> {
  const first = x[0] as number
  const span = (x[x.length - 1] as number) - first
  return Array.from({ length: count }, (_, i) => {
    const t = first + (span * i) / (count - 1)
    return { t, x: px(t) }
  })
}

export function LinePlot({
  x,
  series,
  width,
  height,
  highlight,
}: {
  /** milliseconds, ascending */
  x: number[]
  series: PlotSeries[]
  width: number
  height: number
  highlight?: string | null | undefined
}) {
  const [hoverIndex, setHoverIndex] = useState<number | null>(null)

  const geometry = useMemo(() => {
    if (x.length < 2 || width < 40) return null

    const plotWidth = width - k_pad.left - k_pad.right
    const tickCount = Math.max(2, Math.min(5, Math.floor(plotWidth / 140)))
    const plotHeight = height - k_pad.top - k_pad.bottom
    const px = linear([x[0] as number, x[x.length - 1] as number], [k_pad.left, k_pad.left + plotWidth])
    const [minY, maxY] = paddedDomain(series.flatMap((s) => s.values))
    const py = linear([minY, maxY], [k_pad.top + plotHeight, k_pad.top])

    const paths = series.map((s) => {
      const line = s.values
        .map((v, i) => `${i === 0 ? 'M' : 'L'}${px(x[i] as number).toFixed(1)},${py(v).toFixed(1)}`)
        .join(' ')
      const base = py(Math.max(minY, Math.min(maxY, 0))).toFixed(1)
      return {
        ...s,
        line,
        fill: `${line} L${px(x[x.length - 1] as number).toFixed(1)},${base} L${px(x[0] as number).toFixed(1)},${base} Z`,
        lastX: px(x[x.length - 1] as number),
        lastY: py(s.values[s.values.length - 1] as number),
      }
    })

    return {
      paths,
      px,
      py,
      plotWidth,
      zeroY: minY <= 0 && maxY >= 0 ? py(0) : null,
      yTicks: (() => {
        const values = niceTicks(minY, maxY, 4)
        const label = axisLabel((values[1] ?? minY) - (values[0] ?? 0) || maxY - minY)
        return values.map((v) => ({ v, y: py(v), label: label(v) }))
      })(),
      xTicks: xTicks(x, tickCount, px),
      labelX: labelFor((x[x.length - 1] as number) - (x[0] as number)),
    }
  }, [x, series, width, height])

  if (geometry === null) return null

  const onMove = (event: React.MouseEvent<SVGSVGElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect()
    const offset = event.clientX - bounds.left
    let nearest = 0
    let best = Infinity
    for (let i = 0; i < x.length; i += 1) {
      const distance = Math.abs(geometry.px(x[i] as number) - offset)
      if (distance < best) {
        best = distance
        nearest = i
      }
    }
    setHoverIndex(nearest)
  }

  const hovered = hoverIndex !== null ? geometry.px(x[hoverIndex] as number) : null

  return (
    <div className="relative">
      <svg
        width={width}
        height={height}
        className="block overflow-visible"
        onMouseMove={onMove}
        onMouseLeave={() => setHoverIndex(null)}
      >
        <defs>
          {geometry.paths
            .filter((p) => p.area === true)
            .map((p) => (
              <linearGradient key={p.key} id={`fill-${p.key}`} x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor={p.color} stopOpacity={0.22} />
                <stop offset="70%" stopColor={p.color} stopOpacity={0.04} />
                <stop offset="100%" stopColor={p.color} stopOpacity={0} />
              </linearGradient>
            ))}
        </defs>

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
              {tick.label}
            </text>
          </g>
        ))}

        {geometry.zeroY !== null && (
          <line
            x1={k_pad.left}
            y1={geometry.zeroY.toFixed(1)}
            x2={width - k_pad.right}
            y2={geometry.zeroY.toFixed(1)}
            stroke="#8b97ad"
            strokeOpacity={0.3}
            strokeDasharray="2 3"
          />
        )}

        {geometry.xTicks.map((tick, i, all) => (
          <text
            key={`x${tick.t}`}
            x={tick.x.toFixed(1)}
            y={height - 6}
            fontSize={11}
            fill="#5b6780"
            textAnchor={i === 0 ? 'start' : i === all.length - 1 ? 'end' : 'middle'}
          >
            {geometry.labelX(tick.t)}
          </text>
        ))}

        {geometry.paths.map((path) => {
          const dim = highlight !== null && highlight !== undefined && highlight !== path.key
          return (
            <g key={path.key} opacity={dim ? 0.25 : 1} className="transition-opacity duration-200">
              {path.area === true && <path d={path.fill} fill={`url(#fill-${path.key})`} />}
              <path
                d={path.line}
                fill="none"
                stroke={path.color}
                strokeWidth={dim ? 1 : 1.6}
                strokeLinejoin="round"
                strokeLinecap="round"
              />
              <circle cx={path.lastX} cy={path.lastY} r={4.5} fill={path.color} fillOpacity={0.18} />
              <circle cx={path.lastX} cy={path.lastY} r={2.2} fill={path.color} />
            </g>
          )
        })}

        {hovered !== null && (
          <g>
            <line
              x1={hovered.toFixed(1)}
              y1={k_pad.top}
              x2={hovered.toFixed(1)}
              y2={height - k_pad.bottom}
              stroke="#e6ebf5"
              strokeOpacity={0.25}
            />
            {geometry.paths.map((path) => (
              <circle
                key={path.key}
                cx={hovered.toFixed(1)}
                cy={geometry.py(path.values[hoverIndex as number] as number).toFixed(1)}
                r={3}
                fill={path.color}
                stroke="#0b101e"
                strokeWidth={1.5}
              />
            ))}
          </g>
        )}
      </svg>

      {hoverIndex !== null && hovered !== null && (
        <div
          className="pointer-events-none absolute top-0 z-10 min-w-36 rounded-md bg-raised px-2.5 py-2 shadow-[0_12px_32px_-8px_rgba(0,0,0,0.7)]"
          style={{
            left: Math.min(Math.max(hovered - 70, 0), Math.max(width - 150, 0)),
          }}
        >
          <div className="mb-1 text-2xs text-fg-dim">
            {dateTime(x[hoverIndex] as number)}
          </div>
          {series.map((s) => (
            <div key={s.key} className="flex items-center justify-between gap-3 text-xs">
              <span className="flex items-center gap-1.5">
                <span className="size-1.5 rounded-full" style={{ backgroundColor: s.color }} />
                <span className="text-fg-mid">{s.label}</span>
              </span>
              <span className="tnum" style={{ color: s.color }}>
                {compact(s.values[hoverIndex] as number)}
              </span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
