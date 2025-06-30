import { useEffect, useState } from 'react'
import { ApiError } from '@/api/client'

export type Query<T> =
  | { state: 'loading' }
  | { state: 'ready'; data: T }
  | { state: 'failed'; status: number | null; title: string; detail?: string | undefined }

/** Runs a fetch and reports all three outcomes as data.
 *
 *  There is no fourth state for "no backend": a screen that cannot reach
 *  admind has FAILED, and saying so is the point. v2 had a flag that swapped
 *  screens to invented numbers when the backend was absent, and both paths had
 *  to be maintained forever. */
export function useQuery<T>(run: () => Promise<T>, deps: unknown[], refreshMs?: number): Query<T> {
  const [result, setResult] = useState<Query<T>>({ state: 'loading' })

  useEffect(() => {
    let live = true
    const fetchOnce = () => {
      run()
        .then((data) => {
          if (live) setResult({ state: 'ready', data })
        })
        .catch((err: unknown) => {
          if (!live) return
          if (err instanceof ApiError) {
            setResult({
              state: 'failed',
              status: err.status,
              title: err.problem?.title ?? `HTTP ${err.status}`,
              detail: err.problem?.detail,
            })
          } else {
            setResult({
              state: 'failed',
              status: null,
              title: 'admind is not answering',
              detail: err instanceof Error ? err.message : String(err),
            })
          }
        })
    }
    fetchOnce()
    if (refreshMs === undefined) return () => { live = false }
    const timer = setInterval(fetchOnce, refreshMs)
    return () => {
      live = false
      clearInterval(timer)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps)

  return result
}
