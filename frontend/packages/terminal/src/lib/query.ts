import { useEffect, useState } from 'react'
import { ApiError } from '../api/client'
import { LatestOnly } from './latest'

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

/** a null run stays loading; an answer to superseded deps is dropped, and so is one that lands after a later poll's.
 *  `merge` sees the answer on top of what is held, for rows that carry a version */
export function useQuery<T>(
  run: (() => Promise<T>) | null,
  deps: unknown[],
  refreshMs?: number,
  merge?: (held: T, next: T) => T,
): Query<T> {
  const [result, setResult] = useState<Query<T>>({ state: 'loading' })

  useEffect(() => {
    if (run === null) return
    let live = true
    const polls = new LatestOnly()
    const fetchOnce = () => {
      const ticket = polls.begin()
      run().then(
        (data) => {
          if (!live || !polls.accept(ticket)) return
          setResult((held) =>
            held.state === 'ready' && merge !== undefined
              ? { state: 'ready', data: merge(held.data, data) }
              : { state: 'ready', data },
          )
        },
        (err: unknown) => {
          if (live && polls.accept(ticket)) setResult(failure(err))
        },
      )
    }
    fetchOnce()
    if (refreshMs === undefined) return () => void (live = false)
    // a tab nobody is looking at asks nothing of the box, and catches up when it is shown again
    const whenShown = () => {
      if (!document.hidden) fetchOnce()
    }
    const timer = setInterval(whenShown, refreshMs)
    document.addEventListener('visibilitychange', whenShown)
    return () => {
      live = false
      clearInterval(timer)
      document.removeEventListener('visibilitychange', whenShown)
    }
  }, deps)

  return result
}
