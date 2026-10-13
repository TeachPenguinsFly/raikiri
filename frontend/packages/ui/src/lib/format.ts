/** for geometry and sums only; a displayed figure is formatted from its wire string */
export function toNumber(decimal: string | null | undefined): number {
  return num(decimal) ?? 0
}

/** null means the venue published nothing */
export function num(decimal: string | null | undefined): number | null {
  if (decimal === undefined || decimal === null || decimal === '') return null
  const value = Number(decimal)
  return Number.isFinite(value) ? value : null
}

/** pads a wire decimal with zeros, to at least `atLeast` places, and never drops a digit */
export function price(decimal: string, atLeast = 0): string {
  const [whole = '0', fraction = ''] = decimal.split('.')
  const magnitude = Math.abs(Number(whole))
  const places = Math.max(atLeast, magnitude >= 1000 ? 1 : magnitude >= 1 ? 3 : 5)
  return `${whole}.${fraction.padEnd(places, '0')}`
}

/** the most places among the given decimals, so a column of them can line up */
export function placesOf(decimals: Array<string | null>): number {
  return Math.max(0, ...decimals.map((d) => d?.split('.')[1]?.length ?? 0))
}

/** the midpoint of two wire decimals, exact to one place past the finer of them */
export function midPrice(bid: string, ask: string): string {
  const places = Math.max(bid.split('.')[1]?.length ?? 0, ask.split('.')[1]?.length ?? 0) + 1
  return price(((Number(bid) + Number(ask)) / 2).toFixed(places).replace(/0$/, ''))
}

export function spreadBp(bid: number | null, ask: number | null): number | null {
  if (bid === null || ask === null || bid + ask === 0) return null
  return ((ask - bid) / ((bid + ask) / 2)) * 10_000
}

export function fixed(value: number, places = 2): string {
  return value.toLocaleString('en-US', { minimumFractionDigits: places, maximumFractionDigits: places })
}

export function signed(value: number, places = 2): string {
  return `${value >= 0 ? '+' : '−'}${fixed(Math.abs(value), places)}`
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

export function size(value: number): string {
  const magnitude = Math.abs(value)
  if (magnitude >= 1e9) return `${(value / 1e9).toFixed(2)}B`
  if (magnitude >= 1e6) return `${(value / 1e6).toFixed(2)}M`
  if (magnitude >= 1e3) return `${(value / 1e3).toFixed(1)}K`
  if (magnitude >= 1) return value.toFixed(2)
  if (magnitude === 0) return '0'
  return value.toFixed(4)
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
