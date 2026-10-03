import { NavLink } from 'react-router'
import { BrandMark } from '@/components/shared/BrandMark'
import { cn } from '@/lib/format'

const k_nav = [
  { to: '/', label: 'Markets', end: true },
  { to: '/scanner', label: 'Scanner' },
  { to: '/strategies', label: 'Strategies' },
  { to: '/treasury', label: 'Treasury' },
  { to: '/settings', label: 'Settings' },
]

export function SidebarNav() {
  return (
    <aside className="flex w-40 shrink-0 flex-col">
      <div className="flex h-10 items-center px-4">
        <BrandMark className="text-xs" />
      </div>

      <nav className="flex flex-1 flex-col overflow-y-auto py-1">
        {k_nav.map(({ to, label, end }) => (
          <NavLink
            key={to}
            to={to}
            end={end === true}
            className={({ isActive }) =>
              cn(
                'relative flex h-7 items-center px-4 text-xs transition-colors duration-150',
                isActive
                  ? 'font-medium text-fg before:absolute before:inset-y-1 before:left-0 before:w-[2px] before:bg-accent'
                  : 'text-fg-dim hover:text-fg-mid',
              )
            }
          >
            {label}
          </NavLink>
        ))}
      </nav>
    </aside>
  )
}
