import { useEffect, useState } from 'react'
import { ApiError } from '@/api/client'

export type Query<T> =
  | { state: 'loading' }
  | { state: 'ready'; data: T }
  | { state: 'failed'; status: number | null; title: string; detail?: string | undefined }

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
