/** Rendering for the wire's decimal strings.
 *
 *  Every money value in the contract is a decimal STRING, because a price past
 *  2^53 and a nanosecond timestamp both lose their low digits as a JSON
 *  number. So a display value is derived from the string, and `Number` appears
 *  here only where the result is a pixel coordinate or a chart domain — a
 *  rounding error in a y-axis is a rounding error in a y-axis. */

/** A decimal string as a double. For geometry only: never render this back. */
export function toNumber(decimal: string | null | undefined): number {
  if (decimal === null || decimal === undefined || decimal === '') return 0
  const value = Number(decimal)
  return Number.isFinite(value) ? value : 0
}

const money = new Intl.NumberFormat('en-US', {
  minimumFractionDigits: 2,
  maximumFractionDigits: 2,
})

export function usd(value: number): string {
  return `$${money.format(value)}`
}

export function usdSigned(value: number): string {
  const sign = value > 0 ? '+' : value < 0 ? '−' : ''
  return `${sign}$${money.format(Math.abs(value))}`
}

/** Axis labels, where four characters is the whole budget. */
export function compact(value: number): string {
  const magnitude = Math.abs(value)
  if (magnitude >= 1e9) return `${(value / 1e9).toFixed(1)}B`
  if (magnitude >= 1e6) return `${(value / 1e6).toFixed(1)}M`
  if (magnitude >= 1e3) return `${(value / 1e3).toFixed(1)}k`
  if (magnitude === 0) return '0'
  if (magnitude < 1) return value.toFixed(2)
  return value.toFixed(0)
}

/** A size in the order book, where the column is under sixty pixels wide.
 *
 *  A contract quantity spans nine orders of magnitude across one venue — 0.001
 *  of a BTC contract and 470117.000 of a cheap alt sit in the same column — so
 *  a fixed number of decimals renders one of them unreadable. Above a thousand
 *  the exact figure is not what the eye is there for; the shape of the book is.
 *
 *  Distinct from `compact` above, which is an axis label and gets four
 *  characters. Here two significant decimals survive, because a taker compares
 *  1.2K against 1.9K and rounding both to 1K throws away the comparison. */
export function size(value: number): string {
  const magnitude = Math.abs(value)
  if (magnitude >= 1e9) return `${(value / 1e9).toFixed(2)}B`
  if (magnitude >= 1e6) return `${(value / 1e6).toFixed(2)}M`
  if (magnitude >= 1e3) return `${(value / 1e3).toFixed(1)}K`
  if (magnitude >= 1) return value.toFixed(2)
  if (magnitude === 0) return '0'
  // Below one the leading digits ARE the value, so they are kept whatever the
  // width costs — a size shown as "0.00" reads as an empty level.
  return value.toFixed(4)
}

/** A holding, in its own units. Only the denomination gets a currency mark;
 *  0.001 BTC formatted as money is "$0.00", which reads as nothing held. */
export function quantity(value: number, asset: string, denomination: string): string {
  if (asset === denomination) return usd(value)
  const magnitude = Math.abs(value)
  const digits = magnitude === 0 ? 2 : magnitude < 1 ? 8 : magnitude < 1000 ? 4 : 2
  return value.toLocaleString('en-US', {
    minimumFractionDigits: 2,
    maximumFractionDigits: digits,
  })
}

export function percent(fraction: number): string {
  return `${(fraction * 100).toFixed(1)}%`
}

/** Nanoseconds to milliseconds. BigInt because 1.79e18 is past 2^53 and the
 *  low digits are what distinguish two events in the same second. */
export function nanosToMillis(nanos: string): number {
  return Number(BigInt(nanos) / 1_000_000n)
}

export function shortDate(millis: number): string {
  return new Date(millis).toLocaleDateString(undefined, { month: 'short', day: 'numeric' })
}

/** Time of day with seconds. What a series minutes long needs — a date format
 *  gives it five identical labels. */
export function clockTime(millis: number): string {
  return new Date(millis).toLocaleTimeString(undefined, {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  })
}

export function dateTime(millis: number): string {
  return new Date(millis).toLocaleString(undefined, {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  })
}

/** Class names, without a dependency. Falsy entries drop out. */
export function cn(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ')
}
