import { create } from 'zustand'

export interface EquityPoint {
  t: number
  value: number
}

/** Roughly two hours at the treasury's poll rate. A terminal left open for a
 *  week should not grow a series nobody scrolls back through. */
const k_max_points = 1500

interface EquityState {
  points: EquityPoint[]
  /** The last figure each account reported, whether or not it reported this
   *  round. This is the whole reason the store exists. */
  last: Record<string, number>
  record: (t: number, reporting: Record<string, number>) => void
  reset: () => void
}

/** The total across venues, over time.
 *
 *  CARRIED FORWARD PER ACCOUNT, and that is not a refinement — it is the
 *  difference between a chart and a lie. Every account is polled on its own
 *  thirty-second cycle, so at any instant some of them have not answered yet;
 *  summing only the ones that did makes the total crater and recover, drawing a
 *  vertical cliff where nothing happened. An account holds its last figure
 *  until it reports a new one.
 *
 *  An account contributes nothing until its FIRST report, so a venue coming up
 *  ramps in rather than appearing as a step out of nowhere.
 *
 *  The series lives only as long as the page: nothing persists a balance, and a
 *  history assembled here would be a second answer to a question the venue
 *  answers itself. When snapshots are stored, this reads them instead.
 */
export const useEquity = create<EquityState>((set) => ({
  points: [],
  last: {},
  record: (t, reporting) =>
    set((state) => {
      const last = { ...state.last, ...reporting }
      const value = Object.values(last).reduce((sum, v) => sum + v, 0)
      const previous = state.points[state.points.length - 1]
      // One point per distinct instant. React renders more often than the
      // venues answer, and a series with fifty identical samples per second
      // draws the same line while making every hover test scan them.
      if (previous !== undefined && t - previous.t < 1000 && previous.value === value) {
        return { last }
      }
      const points = [...state.points, { t, value }]
      return { last, points: points.slice(-k_max_points) }
    }),
  reset: () => set({ points: [], last: {} }),
}))
