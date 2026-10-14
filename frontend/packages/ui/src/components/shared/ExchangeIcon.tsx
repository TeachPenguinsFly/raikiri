import { cn } from '../../lib/format'
import { useVenueBrand } from './VenueBrands'

/** every file in assets/exchanges is a venue's mark, keyed by its file name */
const k_icons: Record<string, string> = Object.fromEntries(
  Object.entries(
    import.meta.glob<string>('../../assets/exchanges/*.{png,svg}', { eager: true, import: 'default' }),
  ).map(([path, url]) => [path.slice(path.lastIndexOf('/') + 1, path.lastIndexOf('.')), url]),
)

export function ExchangeIcon({ venue, className }: { venue: string; className?: string }) {
  const c = cn('inline-block shrink-0', className)
  const brand = useVenueBrand(venue)
  const icon = brand?.icon ?? k_icons[venue]
  if (icon) {
    return <img src={icon} alt={brand?.name ?? venue} className={cn(c, 'rounded-sm object-cover')} />
  }
  return (
    <svg viewBox="0 0 24 24" className={c} role="img" aria-label={venue}>
      <rect width="24" height="24" rx="5" fill="#475569" />
      <text x="12" y="16" textAnchor="middle" fontSize="11" fontWeight="700" fill="#fff" fontFamily="system-ui">
        {venue.slice(0, 2).toUpperCase()}
      </text>
    </svg>
  )
}
