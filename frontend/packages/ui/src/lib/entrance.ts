import { useEffect, useRef, useState, type CSSProperties } from 'react'

/** covers the longest stagger plus the animation; later refreshes do not animate */
const k_entrance_window_ms = 700
const k_stagger_ms = 25
const k_stagger_rows = 10

/** true only just after ready first turns true; a new resetKey re-arms it */
export function useEntranceOnce(ready: boolean, resetKey?: unknown): boolean {
  const state = useRef<{ key: unknown; armedAt: number | null }>({ key: resetKey, armedAt: null })
  if (state.current.key !== resetKey) state.current = { key: resetKey, armedAt: null }
  if (ready && state.current.armedAt === null) state.current.armedAt = performance.now()

  const armedAt = state.current.armedAt
  const active = armedAt !== null && performance.now() - armedAt < k_entrance_window_ms

  const [, bump] = useState(0)
  useEffect(() => {
    if (!active) return
    const t = window.setTimeout(() => bump((n) => n + 1), k_entrance_window_ms + 20)
    return () => window.clearTimeout(t)
  }, [active, resetKey])

  return active
}

export function enterDelay(index: number): CSSProperties {
  return { '--enter-delay': `${Math.min(index, k_stagger_rows) * k_stagger_ms}ms` } as CSSProperties
}
