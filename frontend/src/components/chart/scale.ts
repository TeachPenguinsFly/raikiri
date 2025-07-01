import { useLayoutEffect, useRef, useState } from 'react'

/** The element's width in CSS pixels, tracked. An SVG chart needs a real width
 *  before it can place anything, and a viewBox that stretches would make the
 *  strokes and the type scale with the container. */
export function useMeasuredWidth<T extends HTMLElement>(): [React.RefObject<T | null>, number] {
  const ref = useRef<T>(null)
  const [width, setWidth] = useState(0)

  useLayoutEffect(() => {
    const element = ref.current
    if (element === null) return
    // Measured once, synchronously, before the observer: the first callback is
    // delivered on a later task, and a chart that waits for it is a chart that
    // is blank in any context where that task does not run.
    setWidth(Math.round(element.getBoundingClientRect().width))
    const observer = new ResizeObserver((entries) => {
      const measured = entries[0]?.contentRect.width
      if (measured !== undefined && measured > 0) setWidth(Math.round(measured))
    })
    observer.observe(element)
    return () => observer.disconnect()
  }, [])

  return [ref, width]
}

/** Roughly `count` round values spanning the domain. Gridlines land on numbers
 *  a person would have chosen — 0, 25, 50 — rather than on the domain divided
 *  into equal parts, which produces 17.3333. */
export function niceTicks(min: number, max: number, count: number): number[] {
  const span = max - min
  if (span <= 0) return [min]
  const rough = span / count
  const magnitude = 10 ** Math.floor(Math.log10(rough))
  const normalised = rough / magnitude
  const step = (normalised >= 5 ? 10 : normalised >= 2 ? 5 : normalised >= 1 ? 2 : 1) * magnitude
  const ticks: number[] = []
  for (let value = Math.ceil(min / step) * step; value <= max + step * 1e-3; value += step) {
    ticks.push(value)
  }
  return ticks
}

/** A linear map from a data domain to a pixel range. */
export function linear(
  domain: [number, number],
  range: [number, number],
): (value: number) => number {
  const [d0, d1] = domain
  const [r0, r1] = range
  const span = d1 - d0 || 1
  return (value) => r0 + ((value - d0) / span) * (r1 - r0)
}

/** The domain to draw, padded so the extremes are not on the frame, and
 *  including zero when the data straddles it — a PnL chart whose baseline is
 *  off-screen reads as a loss that never happened. */
export function paddedDomain(values: number[], includeZero: boolean): [number, number] {
  if (values.length === 0) return [0, 1]
  let min = Math.min(...values)
  let max = Math.max(...values)
  if (includeZero) {
    min = Math.min(min, 0)
    max = Math.max(max, 0)
  }
  if (min === max) {
    const nudge = Math.abs(min) * 0.05 || 1
    return [min - nudge, max + nudge]
  }
  const padding = (max - min) * 0.12
  return [min - padding, max + padding]
}
