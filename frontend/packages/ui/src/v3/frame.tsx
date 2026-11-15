import { createContext, useContext, useEffect, useRef, useState, type ComponentType, type ReactNode } from 'react'
import { Link, NavLink, useLocation } from 'react-router'
import { cn } from '../lib/format'
import { Glyph } from './ui'
import './v3.css'

export interface NavItem {
  to: string
  label: string
  /** the paths the item stands for; its own prefix otherwise */
  match?: RegExp
  glyph?: string
}

/** what a product hands the shell: its route tree, mounted under the product's prefix */
export interface ProductModule {
  Root: ComponentType
}

/** the TPF Labs penguin, drawn in the text colour */
function Penguin({ className }: { className?: string }) {
  return (
    <svg viewBox="56 38.75 134 191.25" className={className} aria-hidden="true">
      <path
        fill="currentColor"
        fillRule="evenodd"
        d="M190 66 L162 54 C152 34 116 32 102 58 C88 86 94 118 90 146 C86 178 74 206 56 230 C92 224 116 206 130 182 C116 170 112 146 116 122 C118 108 124 96 134 88 C160 112 176 160 168 214 C182 166 176 120 150 94 C154 82 158 74 162 70 Z M132 54 a6 6 0 1 0 12 0 a6 6 0 1 0 -12 0 Z"
      />
    </svg>
  )
}

/** the shell's product switcher, which every product's frame shows beside the brand */
export const SwitcherSlot = createContext<ReactNode>(null)

const k_tab_on =
  'text-fg after:absolute after:inset-x-2.5 after:-bottom-[9px] after:h-px after:bg-accent after:shadow-[0_0_8px_rgb(var(--v3-glow))]'
const k_tab_off = 'text-fg-dim hover:text-fg-mid'

/** a button that drops a list under it; a click outside, Escape or a new page closes it */
export function Menu({ label, className, children }: { label: ReactNode; className?: string; children: ReactNode }) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  const { pathname } = useLocation()
  useEffect(() => setOpen(false), [pathname])
  useEffect(() => {
    if (!open) return
    const off = (e: MouseEvent) => box.current !== null && !box.current.contains(e.target as Node) && setOpen(false)
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setOpen(false)
    window.addEventListener('mousedown', off)
    window.addEventListener('keydown', esc)
    return () => {
      window.removeEventListener('mousedown', off)
      window.removeEventListener('keydown', esc)
    }
  }, [open])
  return (
    <div ref={box} className="relative">
      <button
        type="button"
        aria-expanded={open}
        onClick={() => setOpen((o) => !o)}
        className={cn(
          'relative flex h-7 cursor-pointer items-center gap-1 rounded-md px-2.5 text-xs transition-colors duration-200',
          className,
        )}
      >
        {label}
        <Glyph name="down" className={cn('size-3 transition-transform duration-200', open && 'rotate-180')} />
      </button>
      {open && (
        <div
          onClick={() => setOpen(false)}
          className="v3-pop absolute top-9 left-0 z-50 w-44 rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_20px_50px_-10px_rgb(0_0_0/0.9)]"
        >
          {children}
        </div>
      )}
    </div>
  )
}

/** one row of a menu: a glyph and a label */
export function MenuItem({ to, label, glyph }: { to: string; label: string; glyph?: string }) {
  return (
    <NavLink
      to={to}
      className={({ isActive }) =>
        cn('flex items-center gap-2.5 rounded-sm px-3 py-2 transition-colors', isActive ? 'bg-hover' : 'hover:bg-hover')
      }
    >
      {glyph !== undefined && <Glyph name={glyph} className="size-3.5 text-fg-dim" />}
      <span className="text-xs text-fg">{label}</span>
    </NavLink>
  )
}

const on = (item: NavItem, pathname: string) => item.match?.test(pathname) ?? pathname.startsWith(item.to)

/** the top bar over a product's pages: brand, switcher, the product's pages and tools, then whatever it adds */
export function Frame({
  nav = [],
  tools = [],
  center,
  right,
  banner,
  children,
}: {
  nav?: readonly NavItem[]
  /** the product's own pages, out of the way of browsing */
  tools?: readonly NavItem[]
  center?: ReactNode
  right?: ReactNode
  banner?: ReactNode
  children?: ReactNode
}) {
  const { pathname } = useLocation()
  const switcher = useContext(SwitcherSlot)
  return (
    <div className="v3 v3-page flex h-dvh w-screen flex-col overflow-hidden">
      <header className="relative flex h-11 shrink-0 items-center gap-4 border-b border-line px-3">
        <Link to="/" className="flex shrink-0 items-center" aria-label="TPF Labs">
          <Penguin className="h-6 w-auto text-fg" />
        </Link>
        <nav className="flex items-center gap-0.5">
          {nav.map((item) => (
            <NavLink
              key={item.to}
              to={item.to}
              className={cn(
                'relative h-7 rounded-md px-2.5 text-xs leading-7 transition-colors duration-200',
                on(item, pathname) ? k_tab_on : k_tab_off,
              )}
            >
              {item.label}
            </NavLink>
          ))}
          {tools.length > 0 && (
            <Menu label="Tools" className={tools.some((t) => on(t, pathname)) ? k_tab_on : k_tab_off}>
              {tools.map((t) => (
                <MenuItem key={t.to} to={t.to} label={t.label} {...(t.glyph !== undefined && { glyph: t.glyph })} />
              ))}
            </Menu>
          )}
        </nav>
        {/* on a wide screen the middle is the page's middle, not what the nav and the right side leave */}
        {center !== undefined && (
          <div className="flex flex-1 justify-center max-[900px]:hidden min-[1200px]:pointer-events-none min-[1200px]:absolute min-[1200px]:inset-x-0 min-[1200px]:top-0 min-[1200px]:h-full min-[1200px]:items-center">
            <div className="pointer-events-auto w-[min(36rem,42vw)]">{center}</div>
          </div>
        )}
        {(right !== undefined || switcher !== null) && (
          <div className="ml-auto flex items-center gap-3">
            {right}
            {switcher}
          </div>
        )}
      </header>
      {banner}
      <main className="min-h-0 flex-1">{children}</main>
    </div>
  )
}
