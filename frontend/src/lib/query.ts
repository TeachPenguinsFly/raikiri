import { useEffect, useState } from 'react'
import { ApiError } from '@/api/client'

export type Query<T> =
  | { state: 'loading' }
  | { state: 'ready'; data: T }
  | { state: 'failed'; status: number | null; title: string; detail?: string | undefined }

const failure = (err: unknown): Query<never> =>
  err instanceof ApiError
    ? {
        state: 'failed',
        status: err.status,
        title: err.problem?.title ?? `HTTP ${err.status}`,
        detail: err.problem?.detail,
      }
    : {
        state: 'failed',
        status: null,
        title: 'admind is not answering',
        detail: err instanceof Error ? err.message : String(err),
      }

/** a null run stays loading; an answer to superseded deps is dropped */
export function useQuery<T>(
  run: (() => Promise<T>) | null,
  deps: unknown[],
  refreshMs?: number,
): Query<T> {
  const [result, setResult] = useState<Query<T>>({ state: 'loading' })

  useEffect(() => {
    if (run === null) return
    let live = true
    const fetchOnce = () => {
      run().then(
        (data) => live && setResult({ state: 'ready', data }),
        (err: unknown) => live && setResult(failure(err)),
      )
    }
    fetchOnce()
    const timer = refreshMs === undefined ? undefined : setInterval(fetchOnce, refreshMs)
    return () => {
      live = false
      clearInterval(timer)
    }
  }, deps)

  return result
}
