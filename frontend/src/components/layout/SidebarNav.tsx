import { NavLink } from 'react-router'
import { BrandMark } from '@/components/shared/BrandMark'
import { Icon } from '@/components/shared/Icon'
import { cn } from '@/lib/utils'

const NAV: Array<{
  to: string
  label: string
  icon: (p: { className?: string }) => React.ReactElement
  end?: boolean
}> = [
  { to: '/', label: 'Markets', icon: Icon.Markets, end: true },
  { to: '/strategies', label: 'Strategies', icon: Icon.Bot },
  { to: '/treasury', label: 'Treasury', icon: Icon.Landmark },
  { to: '/settings', label: 'Settings', icon: Icon.Settings },
]

export function SidebarNav() {
  return (
    <aside className="flex w-[200px] shrink-0 flex-col">
      <div className="flex h-14 items-center px-4">
        <BrandMark className="text-[15px]" />
      </div>

      <nav className="flex flex-1 flex-col gap-0.5 overflow-y-auto px-2.5 py-2">
        {NAV.map(({ to, label, icon: Glyph, end }) => (
          <NavLink
            key={to}
            to={to}
            end={end === true}
            className={({ isActive }) =>
              cn(
                'group flex h-9 items-center gap-2.5 rounded-[4px] px-3 text-[13px] font-semibold transition-colors',
                isActive ? 'bg-white/8 text-fg' : 'text-fg-mid hover:bg-white/4 hover:text-fg',
              )
            }
          >
            {({ isActive }) => (
              <>
                <Glyph
                  className={cn(
                    'size-4',
                    isActive ? 'text-accent' : 'text-fg-dim group-hover:text-fg-mid',
                  )}
                />
                {label}
                {isActive && <span className="ml-auto size-1.5 rounded-full bg-accent" />}
              </>
            )}
          </NavLink>
        ))}
      </nav>
    </aside>
  )
}
