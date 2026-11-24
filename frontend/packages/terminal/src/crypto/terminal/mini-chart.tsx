import { useEffect, useMemo, useRef, useState } from 'react'
import {
  ColorType,
  createChart,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  LineStyle,
  type IChartApi,
  type ISeriesApi,
  type UTCTimestamp,
} from 'lightweight-charts'

export type Tone = 'up' | 'down' | 'accent' | 'amber' | 'fg-mid'

export interface MiniSeries {
  name: string
  kind: 'line' | 'bars'
  /** a theme colour, or one per value for bars that change colour with their sign */
  tone: Tone | ((value: number) => Tone)
  /** [unix seconds, value], oldest first */
  points: ReadonlyArray<readonly [number, number]>
  format: (value: number) => string
  /** the left axis for a second unit, or faint along the bottom with no axis */
  scale?: 'left' | 'under'
}

// the canvas takes no css variables, so the theme's colours are read off the element once it is in the page
const colorOf = (el: HTMLElement, tone: string) =>
  getComputedStyle(el).getPropertyValue(`--color-${tone}`).trim() || '#93a3b8'

function faded(hex: string, alpha: number): string {
  const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex)
  return m === null ? hex : `rgba(${parseInt(m[1]!, 16)},${parseInt(m[2]!, 16)},${parseInt(m[3]!, 16)},${alpha})`
}

const toneAt = (s: MiniSeries, value: number): Tone => (typeof s.tone === 'function' ? s.tone(value) : s.tone)

/** a small history panel: its title, each series' value under the crosshair or the latest, and the chart */
export function MiniChart({ title, series, logo = false }: { title: string; series: MiniSeries[]; logo?: boolean }) {
  const box = useRef<HTMLDivElement>(null)
  const chart = useRef<IChartApi | null>(null)
  const drawn = useRef<Array<ISeriesApi<'Line'> | ISeriesApi<'Histogram'>>>([])
  const [at, setAt] = useState<number | null>(null)
  const shape = series.map((s) => `${s.name}:${s.kind}:${s.scale ?? ''}`).join('|')

  useEffect(() => {
    const el = box.current
    if (el === null) return
    const accent = colorOf(el, 'accent')
    const cross = { color: faded(accent, 0.35), width: 1 as const, style: LineStyle.Dashed, labelVisible: false }
    const c = createChart(el, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: 'transparent' },
        textColor: colorOf(el, 'fg-dim'),
        fontFamily: "'Inter', system-ui, sans-serif",
        fontSize: 10,
        attributionLogo: logo,
      },
      grid: { vertLines: { visible: false }, horzLines: { color: 'rgba(255,255,255,0.035)' } },
      rightPriceScale: { borderVisible: false, scaleMargins: { top: 0.12, bottom: 0.1 } },
      leftPriceScale: {
        visible: series.some((s) => s.scale === 'left'),
        borderVisible: false,
        scaleMargins: { top: 0.12, bottom: 0.1 },
      },
      timeScale: { borderVisible: false, timeVisible: true, secondsVisible: false, rightOffset: 2 },
      crosshair: { mode: CrosshairMode.Magnet, vertLine: { ...cross, labelVisible: true }, horzLine: cross },
      handleScale: { axisPressedMouseMove: false },
    })
    chart.current = c
    drawn.current = series.map((s) => {
      const common = {
        priceFormat: { type: 'custom' as const, formatter: s.format, minMove: 1e-8 },
        priceScaleId: s.scale === 'left' ? 'left' : s.scale === 'under' ? '' : 'right',
        priceLineVisible: false,
        lastValueVisible: false,
      }
      const made =
        s.kind === 'bars'
          ? c.addSeries(HistogramSeries, common)
          : c.addSeries(LineSeries, {
              ...common,
              color: colorOf(el, toneAt(s, 0)),
              lineWidth: 2,
              crosshairMarkerRadius: 3,
            })
      if (s.scale === 'under') made.priceScale().applyOptions({ scaleMargins: { top: 0.75, bottom: 0 } })
      return made
    })
    const onMove = (p: Parameters<Parameters<IChartApi['subscribeCrosshairMove']>[0]>[0]) =>
      setAt(typeof p.time === 'number' ? p.time : null)
    c.subscribeCrosshairMove(onMove)
    return () => {
      c.remove()
      chart.current = null
      drawn.current = []
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shape, logo])

  const filled = useRef('')
  useEffect(() => {
    const el = box.current
    if (el === null) return
    series.forEach((s, i) => {
      const alpha = s.scale === 'under' ? 0.3 : 0.75
      drawn.current[i]?.setData(
        s.points.map(([t, value]) =>
          s.kind === 'bars'
            ? { time: t as UTCTimestamp, value, color: faded(colorOf(el, toneAt(s, value)), alpha) }
            : { time: t as UTCTimestamp, value },
        ),
      )
    })
    // a refresh keeps the reader's zoom; only a chart drawn anew is fitted
    if (filled.current !== shape) chart.current?.timeScale().fitContent()
    filled.current = shape
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [series, shape, logo])

  const byTime = useMemo(() => series.map((s) => new Map(s.points)), [series])

  return (
    <section className="flex h-52 min-w-0 flex-col rounded-md bg-canvas/40 shadow-[0_0_0_1px_var(--color-line)]">
      <div className="flex shrink-0 flex-wrap items-baseline gap-x-3 px-3 pt-2 text-xs">
        <span className="font-medium text-fg">{title}</span>
        {series.map((s, i) => {
          const value = at === null ? s.points.at(-1)?.[1] : byTime[i]?.get(at)
          if (value === undefined) return null
          return (
            <span key={s.name} className="flex items-baseline gap-1.5 text-fg-dim">
              {s.name}
              <span className="mono" style={{ color: `var(--color-${toneAt(s, value)})` }}>
                {s.format(value)}
              </span>
            </span>
          )
        })}
      </div>
      <div ref={box} className="min-h-0 flex-1" />
    </section>
  )
}
