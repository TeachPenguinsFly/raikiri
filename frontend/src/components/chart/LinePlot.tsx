import { useMemo, useState } from 'react'
import { clockTime, compact, dateTime, shortDate } from '@/lib/format'
import { linear, niceTicks, paddedDomain } from './scale'

export interface PlotSeries {
  key: string
  label: string
  color: string
  /** One value per x. Same length as `x`, aligned by index. */
  values: number[]
  /** Fill under the line. At most one series should ask for it. */
  area?: boolean | undefined
}

const PAD = { top: 12, right: 52, bottom: 22, left: 8 }

/** Ticks are labelled from the STEP between them, not from their magnitude.
 *
 *  `compact` renders 2,850 and 2,900 both as "2.9k", and an axis whose labels
 *  repeat is an axis that says nothing. */
function axisLabel(step: number): (value: number) => string {
  if (step >= 1000) return compact
  const decimals = Math.max(0, Math.min(6, Math.ceil(-Math.log10(step))))
  // A tick can land on -0, and "-0" on an axis reads as a value rather than as
  // the origin. Adding zero folds it back to +0.
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

/** Evenly spaced time labels across the span, ends included. */
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

/** A multi-series line chart, drawn as SVG at native resolution.
 *
 *  The same component draws the treasury's cumulative ledger and a strategy's
 *  PnL: they differ by their series, not by their code. It replaces the two
 *  near-identical charts the previous terminal carried, which had drifted apart
 *  in padding, tick count and hover behaviour. */
export function LinePlot({
  x,
  series,
  width,
  height,
  highlight,
  format = compact,
  baseline = 'zero',
}: {
  /** Milliseconds, ascending. */
  x: number[]
  series: PlotSeries[]
  width: number
  height: number
  /** A series key to emphasise; the rest dim. */
  highlight?: string | null | undefined
  format?: ((value: number) => string) | undefined
  /** Where the vertical axis starts.
   *
   *  `zero` for anything that can be negative — a PnL chart whose baseline is
   *  off-screen reads as a loss that never happened. `data` for a LEVEL, like a
   *  balance: forcing zero onto a series that lives near 2 900 flattens every
   *  move into a straight line at the top of a slab. */
  baseline?: 'zero' | 'data' | undefined
}) {
  const [hoverIndex, setHoverIndex] = useState<number | null>(null)

  const geometry = useMemo(() => {
    if (x.length < 2 || width < 40) return null

    const plotWidth = width - PAD.left - PAD.right
    const tickCount = Math.max(2, Math.min(5, Math.floor(plotWidth / 140)))
    const plotHeight = height - PAD.top - PAD.bottom
    const px = linear([x[0] as number, x[x.length - 1] as number], [PAD.left, PAD.left + plotWidth])
    const [minY, maxY] = paddedDomain(series.flatMap((s) => s.values), baseline !== 'data')
    const py = linear([minY, maxY], [PAD.top + plotHeight, PAD.top])

    const paths = series.map((s) => {
      const line = s.values
        .map((v, i) => `${i === 0 ? 'M' : 'L'}${px(x[i] as number).toFixed(1)},${py(v).toFixed(1)}`)
        .join(' ')
      // The fill reaches the bottom of the PLOT for a level series and the zero
      // line for a signed one — filling a balance chart down to an off-screen
      // zero paints the whole panel.
      const base = (baseline === 'data' ? PAD.top + plotHeight : py(Math.max(minY, Math.min(maxY, 0)))).toFixed(1)
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
      // The span decides the label. Five identical timestamps is what a
      // minute-long series gets from a date format, and five dates with a
      // time-of-day is what a month-long one gets from a clock.
      labelX: labelFor((x[x.length - 1] as number) - (x[0] as number)),
    }
  }, [x, series, width, height, baseline])

  if (geometry === null) return null

  // Nearest sample to the pointer. A crosshair that interpolates would read a
  // value off the line that no event ever produced.
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
              x1={PAD.left}
              y1={tick.y.toFixed(1)}
              x2={width - PAD.right}
              y2={tick.y.toFixed(1)}
              stroke="#ffffff"
              strokeOpacity={0.045}
            />
            <text
              x={width - PAD.right + 8}
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
            x1={PAD.left}
            y1={geometry.zeroY.toFixed(1)}
            x2={width - PAD.right}
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
              y1={PAD.top}
              x2={hovered.toFixed(1)}
              y2={height - PAD.bottom}
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
                {format(s.values[hoverIndex] as number)}
              </span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
