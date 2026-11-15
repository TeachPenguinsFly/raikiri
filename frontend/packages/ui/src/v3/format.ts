/** a decimal the venues send as a string, or undefined when it is empty or not a number */
export const n = (s: string | undefined): number | undefined => {
  if (s === undefined || s === '') return undefined
  const v = Number(s)
  return Number.isFinite(v) ? v : undefined
}

/** a price in cents the way the book prints it: 34.0¢, 4.5¢ */
export const c = (p: number | undefined, places = 1): string =>
  p === undefined ? '—' : `${(p * 100).toFixed(places)}¢`

export function compact(v: number | undefined, dollar = false): string {
  if (v === undefined || !Number.isFinite(v)) return '—'
  const s = dollar ? '$' : ''
  const a = Math.abs(v)
  const sign = v < 0 ? '−' : ''
  if (a >= 1e9) return `${sign}${s}${(a / 1e9).toFixed(2)}B`
  if (a >= 1e6) return `${sign}${s}${(a / 1e6).toFixed(2)}M`
  if (a >= 1e4) return `${sign}${s}${(a / 1e3).toFixed(1)}K`
  if (a >= 1e3) return `${sign}${s}${(a / 1e3).toFixed(2)}K`
  return `${sign}${s}${a.toFixed(2)}`
}
