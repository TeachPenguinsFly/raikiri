export interface Candle {
  /** seconds, as the chart takes them */
  time: number
  open: number
  high: number
  low: number
  close: number
  ticks: number
}

/** candles of a value that holds until its next point, so a candle opens on the value carried into it; points are
 * [ms, value] in time order */
export function candles(points: ReadonlyArray<readonly [number, number]>, seconds: number, now: number, max: number) {
  const step = seconds * 1000
  if (points.length === 0) return []
  const first = Math.floor((points[0] as readonly [number, number])[0] / step) * step
  const start = Math.max(first, Math.floor(now / step) * step - (max - 1) * step)
  const out: Candle[] = []
  let i = 0
  let cur: number | undefined
  while (i < points.length && (points[i] as readonly [number, number])[0] < start)
    cur = (points[i++] as readonly [number, number])[1]
  for (let t = start; t <= now; t += step) {
    const end = t + step
    let open = cur
    let high = cur ?? -Infinity
    let low = cur ?? Infinity
    let ticks = 0
    while (i < points.length && (points[i] as readonly [number, number])[0] < end) {
      const v = (points[i++] as readonly [number, number])[1]
      if (open === undefined) open = v
      high = Math.max(high, v)
      low = Math.min(low, v)
      cur = v
      ticks += 1
    }
    if (open === undefined || cur === undefined) continue
    out.push({ time: t / 1000, open, high, low, close: cur, ticks })
  }
  return out
}
