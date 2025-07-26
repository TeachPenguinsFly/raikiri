import { useEffect, useRef, useState, type CSSProperties } from 'react'

/** How long entrance classes stay applied after content first renders — long
 *  enough for the longest stagger+duration, short enough that later data
 *  refreshes (rows keyed in fresh) never animate. */
const ENTRANCE_WINDOW_MS = 700
const STAGGER_MS = 25
const STAGGER_CAP = 10

/**
 * True only during the short window after `ready` first flips true — used to
 * run list/card entrance animations exactly once, on the initial
 * skeleton→content swap, never on subsequent data refreshes. Pass `resetKey`
 * to re-arm for a new context (e.g. a different instrument's quote panel).
 */
export function useEntranceOnce(ready: boolean, resetKey?: unknown): boolean {
  const state = useRef<{ key: unknown; armedAt: number | null }>({ key: resetKey, armedAt: null })
  if (state.current.key !== resetKey) state.current = { key: resetKey, armedAt: null }
  if (ready && state.current.armedAt === null) state.current.armedAt = performance.now()

  const armedAt = state.current.armedAt
  const active = armedAt !== null && performance.now() - armedAt < ENTRANCE_WINDOW_MS

  // Re-render once at window end so the classes are removed and rows added by
  // later refreshes mount without any animation.
  const [, bump] = useState(0)
  useEffect(() => {
    if (!active) return
    const t = window.setTimeout(() => bump((n) => n + 1), ENTRANCE_WINDOW_MS + 20)
    return () => window.clearTimeout(t)
  }, [active, resetKey])

  return active
}

/** Stagger delay for row/card `index`, capped at ~10 rows so long lists finish
 *  together instead of trickling. Spread onto the animated element's `style`. */
export function enterDelay(index: number): CSSProperties {
  return { '--enter-delay': `${Math.min(index, STAGGER_CAP) * STAGGER_MS}ms` } as CSSProperties
}

/**
 * True for the first `ms` after mount (or after `resetKey` changes) — the
 * window where an empty feed reads as "warming up" (show skeleton) rather than
 * genuinely absent (show the real no-feed / empty state).
 */
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
