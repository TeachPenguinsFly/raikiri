import { useEffect, useRef, useState, type CSSProperties } from 'react'

/** covers the longest stagger plus duration; later refreshes do not animate */
const ENTRANCE_WINDOW_MS = 700
const STAGGER_MS = 25
const STAGGER_CAP = 10

/** true only just after ready first turns true; resetKey re-arms it */
export function useEntranceOnce(ready: boolean, resetKey?: unknown): boolean {
  const state = useRef<{ key: unknown; armedAt: number | null }>({ key: resetKey, armedAt: null })
  if (state.current.key !== resetKey) state.current = { key: resetKey, armedAt: null }
  if (ready && state.current.armedAt === null) state.current.armedAt = performance.now()

  const armedAt = state.current.armedAt
  const active = armedAt !== null && performance.now() - armedAt < ENTRANCE_WINDOW_MS

  const [, bump] = useState(0)
  useEffect(() => {
    if (!active) return
    const t = window.setTimeout(() => bump((n) => n + 1), ENTRANCE_WINDOW_MS + 20)
    return () => window.clearTimeout(t)
  }, [active, resetKey])

  return active
}

/** stagger delay, capped at about ten rows */
export function enterDelay(index: number): CSSProperties {
  return { '--enter-delay': `${Math.min(index, STAGGER_CAP) * STAGGER_MS}ms` } as CSSProperties
}

/** true for the first ms after mount or a resetKey change, while an empty feed means warming up */
export function useWarmup(ms: number, resetKey?: unknown): boolean {
  const state = useRef<{ key: unknown; start: number }>({ key: resetKey, start: performance.now() })
  if (state.current.key !== resetKey) state.current = { key: resetKey, start: performance.now() }
  const warm = performance.now() - state.current.start < ms

  const [, bump] = useState(0)
  useEffect(() => {
    if (!warm) return
    const remain = state.current.start + ms - performance.now()
    const t = window.setTimeout(() => bump((n) => n + 1), Math.max(remain, 0) + 20)
    return () => window.clearTimeout(t)
  }, [warm, resetKey, ms])

  return warm
}
