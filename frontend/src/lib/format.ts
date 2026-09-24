/** money values arrive as decimal strings; convert to number only for geometry */

/** for geometry only; do not render this back */
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

export function compact(value: number): string {
  const magnitude = Math.abs(value)
  if (magnitude >= 1e9) return `${(value / 1e9).toFixed(1)}B`
  if (magnitude >= 1e6) return `${(value / 1e6).toFixed(1)}M`
  if (magnitude >= 1e3) return `${(value / 1e3).toFixed(1)}k`
  if (magnitude === 0) return '0'
  if (magnitude < 1) return value.toFixed(2)
  return value.toFixed(0)
}

/** order book size; keeps two significant decimals above a thousand */
export function size(value: number): string {
  const magnitude = Math.abs(value)
  if (magnitude >= 1e9) return `${(value / 1e9).toFixed(2)}B`
  if (magnitude >= 1e6) return `${(value / 1e6).toFixed(2)}M`
  if (magnitude >= 1e3) return `${(value / 1e3).toFixed(1)}K`
  if (magnitude >= 1) return value.toFixed(2)
  if (magnitude === 0) return '0'
  return value.toFixed(4)
}

/** only the denomination gets a currency mark */
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

/** bigint because nanosecond stamps exceed 2^53 */
export function nanosToMillis(nanos: string): number {
  return Number(BigInt(nanos) / 1_000_000n)
}

export function shortDate(millis: number): string {
  return new Date(millis).toLocaleDateString(undefined, { month: 'short', day: 'numeric' })
}

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

export function cn(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ')
}
