import type { ReactNode } from 'react'

export interface DonutSegment {
  key: string
  label: string
  color: string
  /** Non-negative. A donut of signed values is a lie about proportion. */
  value: number
}

/** A ring of proportions.
 *
 *  Segments are drawn as dashes on one circle rather than as arc paths, which
 *  keeps the geometry to two numbers per segment and lets the growth animate as
 *  a single CSS transition. Each arc gives back a little length at both ends so
 *  the round caps of neighbours do not touch. */
export function Donut({
  segments,
  size,
  thickness = Math.max(14, Math.round(size * 0.11)),
  active,
  onHover,
  center,
}: {
  segments: DonutSegment[]
  size: number
  thickness?: number | undefined
  active?: string | null | undefined
  onHover?: ((key: string | null) => void) | undefined
  center?: ReactNode | undefined
}) {
  const radius = size / 2 - thickness / 2
  const circumference = 2 * Math.PI * radius
  const total = segments.reduce((sum, s) => sum + s.value, 0)
  const gap = total === 0 ? 0 : Math.min(circumference * 0.006, thickness * 0.35)

  let cursor = 0
  const arcs = segments.map((segment) => {
    const fraction = total === 0 ? 0 : segment.value / total
    const start = cursor
    cursor += fraction
    return {
      ...segment,
      fraction,
      offset: start * circumference,
      length: Math.max(fraction * circumference - gap, 0),
    }
  })

  return (
    <div
      className="relative"
      style={{ width: size, height: size }}
      onMouseLeave={() => onHover?.(null)}
    >
      <svg width={size} height={size} className="-rotate-90 overflow-visible">
        <circle
          cx={size / 2}
          cy={size / 2}
          r={radius}
          fill="none"
          stroke="#1d293a"
          strokeOpacity={0.7}
          strokeWidth={thickness}
        />
        {arcs.map((arc) => {
          if (arc.length <= 0) return null
          const isActive = active === arc.key
          return (
            <circle
              key={arc.key}
              cx={size / 2}
              cy={size / 2}
              r={radius}
              fill="none"
              stroke={arc.color}
              strokeWidth={isActive ? thickness + 3 : thickness}
              strokeLinecap="round"
              strokeDasharray={`${arc.length} ${circumference - arc.length}`}
              strokeDashoffset={-arc.offset}
              opacity={active === null || active === undefined || isActive ? 1 : 0.32}
              style={{
                // Grown by a CSS keyframe whose only frame is `from`, so the
                // ring's final geometry is the element's own attributes and no
                // fill mode is needed. Anything that holds the `from` frame —
                // a stagger delay, `both` — leaves the arc at zero length for
                // as long as it holds, and a chart that is blank until an
                // animation runs is a chart that is blank wherever one does
                // not.
                ['--circumference' as string]: `${circumference}`,
                animation: 'donut-grow 850ms cubic-bezier(0.16,1,0.3,1)',
                transition: 'stroke-width 180ms ease-out, opacity 180ms ease-out',
                filter: isActive ? `drop-shadow(0 0 10px ${arc.color})` : undefined,
                cursor: onHover === undefined ? 'default' : 'pointer',
              }}
              onMouseEnter={() => onHover?.(arc.key)}
            />
          )
        })}
      </svg>
      {center !== undefined && (
        <div className="pointer-events-none absolute inset-0 flex flex-col items-center justify-center text-center">
          {center}
        </div>
      )}
    </div>
  )
}
