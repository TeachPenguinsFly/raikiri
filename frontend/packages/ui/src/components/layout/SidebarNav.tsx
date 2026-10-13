import type { ReactNode } from 'react'
import { NavLink } from 'react-router'
import { cn } from '../../lib/format'
import type { AppModule } from './AppShell'

export function SidebarNav({
  brand,
  modules,
}: {
  brand: ReactNode
  modules: readonly Pick<AppModule, 'path' | 'label' | 'icon'>[]
}) {
  return (
    <aside className="flex w-40 shrink-0 flex-col">
      <div className="flex h-10 items-center px-4">{brand}</div>

      <nav className="flex flex-1 flex-col overflow-y-auto py-1">
        {modules.map(({ path, label, icon }) => (
          <NavLink
            key={path}
            to={path}
            end={path === '/'}
            className={({ isActive }) =>
              cn(
                'relative flex h-7 items-center gap-2 px-4 text-xs transition-colors duration-150',
                isActive
                  ? 'font-medium text-fg before:absolute before:inset-y-1 before:left-0 before:w-[2px] before:bg-accent'
                  : 'text-fg-dim hover:text-fg-mid',
              )
            }
          >
            {icon}
            {label}
          </NavLink>
        ))}
      </nav>
    </aside>
  )
}
