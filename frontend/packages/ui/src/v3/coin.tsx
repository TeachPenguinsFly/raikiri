import { cn } from '../lib/format'
import k_names from '../assets/coins/names.json'

/** every file in assets/coins is a coin's mark, keyed by its ticker; scripts/coin_icons.py refreshes them. Each is
 * a file of its own, fetched when shown: inlined, the 581 marks put a megabyte into the terminal's script */
const k_icons: Record<string, string> = Object.fromEntries(
  Object.entries(
    import.meta.glob<string>('../assets/coins/*.webp', { eager: true, import: 'default', query: '?no-inline' }),
  ).map(([path, url]) => [path.slice(path.lastIndexOf('/') + 1, path.lastIndexOf('.')), url]),
)

/** 1000PEPE and kPEPE are PEPE counted in thousands */
const bare = (symbol: string) => symbol.toUpperCase().replace(/^(1000000|1000|1M|K)(?=[A-Z])/, '')

/** metals that perps list under their iso codes, which no coin list carries */
const k_metals: Record<string, string> = { XAU: 'Gold', XAG: 'Silver', XPT: 'Platinum', XPD: 'Palladium' }

/** what a ticker is called in full, where the coin list knows it */
export function coinName(symbol: string): string | undefined {
  const key = bare(symbol)
  return (k_names as Record<string, string>)[key] ?? k_metals[key]
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
    // the letter is drawn in the disc's own units so it scales with whatever size the disc is given
    <svg viewBox="0 0 20 20" className={cn(c, 'text-fg-mid')} aria-hidden="true">
      <circle cx="10" cy="10" r="10" fill="currentColor" fillOpacity="0.14" />
      <text x="10" y="14" fontSize="11" fontWeight="600" textAnchor="middle" fill="currentColor">
        {bare(symbol).slice(0, 1)}
      </text>
    </svg>
  )
}
