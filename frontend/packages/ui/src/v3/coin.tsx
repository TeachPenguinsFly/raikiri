import { cn } from '../lib/format'
import k_names from '../assets/coins/names.json'

/** every file in assets/coins is a coin's mark, keyed by its ticker; scripts/coin_icons.py refreshes them */
const k_icons: Record<string, string> = Object.fromEntries(
  Object.entries(import.meta.glob<string>('../assets/coins/*.webp', { eager: true, import: 'default' })).map(
    ([path, url]) => [path.slice(path.lastIndexOf('/') + 1, path.lastIndexOf('.')), url],
  ),
)

/** 1000PEPE and kPEPE are PEPE counted in thousands */
const bare = (symbol: string) => symbol.toUpperCase().replace(/^(1000000|1000|1M|K)(?=[A-Z])/, '')

/** what a ticker is called in full, where the coin list knows it */
export function coinName(symbol: string): string | undefined {
  return (k_names as Record<string, string>)[bare(symbol)]
}

/** a coin's round mark, or its first letter on a disc when the coin list has none */
export function CoinIcon({
  symbol,
  className,
  plain = false,
}: {
  symbol: string
  className?: string
  plain?: boolean
}) {
  // plain: a listing admind split off for quoting apart from the coin its ticker names, so the coin's mark would mislead
  const icon = plain ? undefined : k_icons[bare(symbol)]
  const c = cn('inline-block shrink-0 rounded-full', className)
  if (icon !== undefined) return <img src={icon} alt="" className={cn(c, 'object-cover')} />
  return (
    <span
      className={cn(c, 'grid place-items-center bg-raised text-[0.6em] font-semibold text-fg-mid')}
      aria-hidden="true"
    >
      {bare(symbol).slice(0, 1)}
    </span>
  )
}
