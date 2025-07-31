import { NavLink } from 'react-router'
import { BrandMark } from '@/components/shared/BrandMark'
import { Icon } from '@/components/shared/Icon'
import { cn } from '@/lib/utils'

const NAV: Array<{
  to: string
  label: string
  icon: (p: { className?: string; strokeWidth?: number }) => React.ReactElement
  end?: boolean
}> = [
  { to: '/', label: 'Markets', icon: Icon.Markets, end: true },
  { to: '/strategies', label: 'Strategies', icon: Icon.Bot },
  { to: '/treasury', label: 'Treasury', icon: Icon.Landmark },
  { to: '/settings', label: 'Settings', icon: Icon.Settings },
]

/** The product's navigation, and nothing else.
 *
 *  Narrow, quiet, and never brighter than the market data beside it. The
 *  active item is marked by a raised surface and full-strength label rather
 *  than by an accent block: on a screen whose whole job is colour-coded
 *  numbers, a coloured slab in the corner competes for the same attention. */
export function SidebarNav() {
  return (
    <aside className="flex w-[184px] shrink-0 flex-col">
      <div className="flex h-12 items-center px-4">
        <BrandMark className="text-base" />
      </div>

      <nav className="flex flex-1 flex-col gap-0.5 overflow-y-auto px-2 py-1">
        {NAV.map(({ to, label, icon: Glyph, end }) => (
          <NavLink
            key={to}
            to={to}
            end={end === true}
            className={({ isActive }) =>
              cn(
                'group flex h-9 items-center gap-2.5 rounded-md px-2 text-sm transition-colors duration-150',
                isActive
                  ? 'bg-active font-medium text-fg'
                  : 'font-normal text-fg-mid hover:bg-hover hover:text-fg',
              )
            }
          >
            {({ isActive }) => (
              <>
                <Glyph
                  className={cn('size-4', isActive ? 'text-fg' : 'text-fg-dim')}
                  strokeWidth={1.75}
                />
                {label}
              </>
            )}
          </NavLink>
        ))}
      </nav>
    </aside>
  )
}
