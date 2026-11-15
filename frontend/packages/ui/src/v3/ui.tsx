import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type CSSProperties,
  type PointerEvent,
  type ReactNode,
} from 'react'
import { createPortal } from 'react-dom'
import { cn } from '../lib/format'

// the small parts every v3 panel shares; the effects follow aceternity ui's, drawn in css so the app takes no
// animation library

/** --mx and --my on the element under the pointer, which .v3-glow and .v3-spot light from */
export function trackPointer(e: PointerEvent<HTMLElement>) {
  const r = e.currentTarget.getBoundingClientRect()
  e.currentTarget.style.setProperty('--mx', `${e.clientX - r.left}px`)
  e.currentTarget.style.setProperty('--my', `${e.clientY - r.top}px`)
}

/** options in a pill whose highlight slides to the chosen one, after aceternity's animated tabs */
export function Seg<T extends string>({
  options,
  value,
  onChange,
  size = 'sm',
  tone,
  className,
}: {
  options: ReadonlyArray<readonly [T, ReactNode]>
  value: T
  onChange: (value: T) => void
  size?: 'xs' | 'sm' | 'md'
  /** the highlight's colour per option; the raised grey otherwise */
  tone?: (value: T) => string | undefined
  className?: string | undefined
}) {
  const box = useRef<HTMLDivElement>(null)
  const [mark, setMark] = useState<{ left: number; width: number } | null>(null)
  useLayoutEffect(() => {
    const el = box.current?.querySelector<HTMLElement>(`[data-value="${CSS.escape(value)}"]`)
    if (el === null || el === undefined) return setMark(null)
    const measure = () => setMark({ left: el.offsetLeft, width: el.offsetWidth })
    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    return () => ro.disconnect()
  }, [value, options.length])
  const h = size === 'xs' ? 'h-6 text-2xs' : size === 'sm' ? 'h-7 text-xs' : 'h-9 text-sm'
  return (
    <div ref={box} role="tablist" className={cn('relative flex items-center rounded-md bg-canvas/60 p-0.5', className)}>
      {mark !== null && (
        <span
          className={cn(
            'absolute inset-y-0.5 rounded-[5px] transition-[left,width,background-color] duration-300 ease-[cubic-bezier(0.22,1,0.36,1)]',
            tone?.(value) ?? 'bg-raised shadow-[0_1px_0_0_rgb(255_255_255/0.04)_inset,0_2px_8px_-2px_rgb(0_0_0/0.6)]',
          )}
          style={{ left: mark.left, width: mark.width }}
        />
      )}
      {options.map(([v, label]) => (
        <button
          key={v}
          type="button"
          role="tab"
          data-value={v}
          aria-selected={v === value}
          onClick={() => onChange(v)}
          className={cn(
            'relative z-[1] flex-1 rounded-[5px] px-2.5 font-medium whitespace-nowrap transition-colors duration-200',
            h,
            v === value ? 'text-fg' : 'text-fg-dim hover:text-fg-mid',
          )}
        >
          {label}
        </button>
      ))}
    </div>
  )
}

/** a number that runs to its next value instead of jumping */
export function useTween(target: number | undefined, ms = 600): number | undefined {
  const [shown, setShown] = useState(target)
  const from = useRef(target)
  useEffect(() => {
    if (target === undefined || !Number.isFinite(target)) {
      from.current = target
      setShown(target)
      return
    }
    const start = from.current ?? target
    if (start === target) {
      setShown(target)
      return
    }
    const t0 = performance.now()
    let raf = 0
    const step = (t: number) => {
      const k = Math.min(1, (t - t0) / ms)
      const eased = 1 - (1 - k) ** 3
      const v = start + (target - start) * eased
      from.current = v
      setShown(v)
      if (k < 1) raf = requestAnimationFrame(step)
    }
    raf = requestAnimationFrame(step)
    return () => cancelAnimationFrame(raf)
  }, [target, ms])
  return shown
}

export function Ticker({
  value,
  format,
  className,
}: {
  value: number | undefined
  format: (v: number) => string
  className?: string
}) {
  const shown = useTween(value)
  return <span className={cn('num', className)}>{shown === undefined ? '—' : format(shown)}</span>
}

/** text that flashes green or red as its number rises or falls */
export function Flash({
  value,
  children,
  className,
}: {
  value: number | undefined
  children: ReactNode
  className?: string
}) {
  const last = useRef(value)
  const [dir, setDir] = useState<'up' | 'down' | null>(null)
  const [n, setN] = useState(0)
  useEffect(() => {
    const before = last.current
    last.current = value
    if (value === undefined || before === undefined || value === before) return
    setDir(value > before ? 'up' : 'down')
    setN((x) => x + 1)
  }, [value])
  return (
    <span
      key={n}
      className={cn(
        '-mx-0.5 rounded-[3px] px-0.5',
        dir === 'up' && 'v3-tick-up',
        dir === 'down' && 'v3-tick-down',
        className,
      )}
    >
      {children}
    </span>
  )
}

export function Corners({ color }: { color?: string }) {
  return <span className="v3-corners" style={color === undefined ? undefined : ({ '--c': color } as CSSProperties)} />
}

export function Stepper({
  value,
  onChange,
  step,
  min,
  max,
  suffix,
}: {
  value: number
  onChange: (v: number) => void
  step: number
  min: number
  max: number
  suffix?: string
}) {
  const clamp = (v: number) => Math.min(max, Math.max(min, Math.round(v / step) * step))
  const btn =
    'grid size-6 place-items-center rounded-sm text-fg-mid transition-colors hover:bg-hover hover:text-fg active:scale-90'
  return (
    <div className="flex items-center gap-1 rounded-md bg-canvas/60 px-1 py-0.5">
      <button type="button" className={btn} onClick={() => onChange(clamp(value - step))} aria-label="less">
        −
      </button>
      <span className="num min-w-10 text-center text-xs text-fg">
        {Number(value.toFixed(4))}
        {suffix}
      </span>
      <button type="button" className={btn} onClick={() => onChange(clamp(value + step))} aria-label="more">
        +
      </button>
    </div>
  )
}

export function Check({
  checked,
  onChange,
  children,
}: {
  checked: boolean
  onChange: (v: boolean) => void
  children: ReactNode
}) {
  return (
    <label className="flex cursor-pointer items-center gap-2 text-xs text-fg-mid select-none">
      <span
        className={cn(
          'grid size-3.5 place-items-center rounded-[3px] border transition-colors duration-150',
          checked ? 'border-accent bg-accent text-canvas' : 'border-fg-dim/50 bg-transparent',
        )}
      >
        {checked && (
          <svg viewBox="0 0 12 12" className="size-2.5" fill="none" stroke="currentColor" strokeWidth={2}>
            <path d="m2.5 6.5 2.25 2L9.5 3.5" />
          </svg>
        )}
      </span>
      <input type="checkbox" className="sr-only" checked={checked} onChange={(e) => onChange(e.target.checked)} />
      {children}
    </label>
  )
}

export function Skeleton({ className }: { className?: string }) {
  return <div className={cn('v3-shimmer rounded-md bg-raised/40', className)} />
}

/** something that did not work, said plainly: a mark, what went wrong, a line more at most, and what to do next */
export function Problem({
  title,
  detail,
  glyph = 'alert',
  tone = 'down',
  actions,
  page = false,
}: {
  title: string
  detail?: ReactNode
  glyph?: string
  tone?: 'down' | 'amber'
  actions?: ReactNode
  page?: boolean
}) {
  return (
    <div
      role="alert"
      className={cn(
        'flex h-full flex-col items-center justify-center gap-3 p-6 text-center',
        page ? 'min-h-[60vh]' : 'min-h-28',
      )}
    >
      <span
        className={cn(
          'grid place-items-center rounded-full',
          page ? 'size-14' : 'size-9',
          tone === 'down'
            ? 'bg-down/10 text-down shadow-[inset_0_0_0_1px_rgb(239_68_68/0.25)]'
            : 'bg-amber/10 text-amber shadow-[inset_0_0_0_1px_rgb(245_158_11/0.25)]',
        )}
      >
        <Glyph name={glyph} className={page ? 'size-6' : 'size-4'} />
      </span>
      <div className="flex flex-col items-center gap-1">
        <span className={cn('font-medium text-fg', page ? 'text-base' : 'text-sm')}>{title}</span>
        {detail !== undefined && <span className="max-w-sm text-xs text-fg-dim">{detail}</span>}
      </div>
      {actions !== undefined && <div className="flex items-center gap-2 pt-1">{actions}</div>}
    </div>
  )
}

export function Empty({ title, children }: { title: string; children?: ReactNode }) {
  return (
    <div className="flex h-full min-h-24 flex-col items-center justify-center gap-1 p-4 text-center">
      <span className="text-sm font-medium text-fg-mid">{title}</span>
      {children !== undefined && <span className="max-w-72 text-xs text-fg-dim">{children}</span>}
    </div>
  )
}

const k_glyph: Record<string, ReactNode> = {
  plus: <path d="M8 3.5v9M3.5 8h9" />,
  close: <path d="m4.5 4.5 7 7M11.5 4.5l-7 7" />,
  max: <path d="M3 6V3h3M10 3h3v3M13 10v3h-3M6 13H3v-3" />,
  min: <path d="M6 3v3H3M13 6h-3V3M10 13v-3h3M3 10h3v3" />,
  search: (
    <>
      <circle cx="7" cy="7" r="4.25" />
      <path d="m10.25 10.25 3 3" />
    </>
  ),
  chevron: <path d="m6 4 4 4-4 4" />,
  down: <path d="m4 6 4 4 4-4" />,
  grip: (
    <>
      <circle cx="6" cy="4.5" r=".6" />
      <circle cx="10" cy="4.5" r=".6" />
      <circle cx="6" cy="8" r=".6" />
      <circle cx="10" cy="8" r=".6" />
      <circle cx="6" cy="11.5" r=".6" />
      <circle cx="10" cy="11.5" r=".6" />
    </>
  ),
  reset: <path d="M3 8a5 5 0 1 0 1.5-3.5L3 6V3" />,
  candles: <path d="M5 2.5v11M3.75 5h2.5v5.5h-2.5zM11 2.5v11M9.75 4h2.5v4h-2.5z" />,
  line: <path d="m2.5 11 3.5-4 3 2.5 4-5" />,
  star: <path d="m8 2.75 1.6 3.3 3.65.5-2.65 2.55.65 3.6L8 11l-3.25 1.7.65-3.6L2.75 6.55l3.65-.5z" />,
  bolt: <path d="M9 2.5 4 9h4l-1 4.5L12 7H8z" />,
  // a price over time on its axes, for a chart panel
  chart: (
    <>
      <path d="M2.75 2.75v10.5h10.5" />
      <path d="m5.25 10 2.5-3 2 1.75 3.5-4" />
    </>
  ),
  play: <path d="M5.5 3.75v8.5L12.25 8z" />,
  alert: <path d="M8 2.75 14 13H2zM8 6.75v2.75M8 11.25v.25" />,
  // a plug pulled from its socket, for a part of the app that is not running
  plug: (
    <path d="M5.5 2.75v2.5M9 2.75v2.5M3.75 5.25h7v2.5a3.5 3.5 0 0 1-7 0zM7.25 11.25v2M11 9.5l2.5 2.5M13.5 9.5 11 12" />
  ),
  // a buy and a sell crossing, for an order ticket
  trade: <path d="M3.25 5.5h9.25M10 3l2.5 2.5L10 8M12.75 10.5H3.5M6 8l-2.5 2.5L6 13" />,
  layers: <path d="m8 2.75 5.25 2.75L8 8.25 2.75 5.5zM2.75 8.25 8 11l5.25-2.75M2.75 10.75 8 13.5l5.25-2.75" />,
  arrow: <path d="M3.5 8h9M9 4.5 12.5 8 9 11.5" />,
  // the groups of events
  'g-live': (
    <>
      <circle cx="8" cy="8" r="2" />
      <path d="M4.5 4.5a5 5 0 0 0 0 7M11.5 4.5a5 5 0 0 1 0 7" />
    </>
  ),
  'g-trending': <path d="M2.75 11.5 6.5 7.75l2.5 2.5 4.25-4.75M10 5.5h3.25v3.25" />,
  'g-sports': (
    <>
      <circle cx="8" cy="8" r="5.25" />
      <path d="M8 2.75v10.5M2.75 8h10.5" />
    </>
  ),
  'g-politics': <path d="M2.75 13.25h10.5M3.75 6.5h8.5M8 2.75 3.5 5.5h9zM5 6.5v5.25M8 6.5v5.25M11 6.5v5.25" />,
  'g-crypto': (
    <>
      <circle cx="8" cy="8" r="5.25" />
      <path d="M6.5 5.25h2.25a1.25 1.25 0 0 1 0 2.5H6.5h2.6a1.35 1.35 0 0 1 0 2.75H6.5zM7.25 4.25v1M7.25 10.5v1.25" />
    </>
  ),
  'g-economy': <path d="M2.75 12.75 6 9.25l2.5 2 4.75-5.5M10.25 5.75h3v3" />,
  'g-tech': (
    <>
      <rect x="4.25" y="4.25" width="7.5" height="7.5" rx="1" />
      <path d="M6.5 2.25v2M9.5 2.25v2M6.5 11.75v2M9.5 11.75v2M2.25 6.5h2M2.25 9.5h2M11.75 6.5h2M11.75 9.5h2" />
    </>
  ),
  'g-culture': <path d="m8 2.75 1.6 3.3 3.65.5-2.65 2.55.65 3.6L8 11l-3.25 1.7.65-3.6L2.75 6.55l3.65-.5z" />,
  'g-weather': <path d="M4.75 12.25a2.75 2.75 0 0 1-.4-5.47A3.75 3.75 0 0 1 11.6 6a3.1 3.1 0 0 1-.35 6.25z" />,
  'g-other': (
    <>
      <circle cx="4" cy="8" r=".75" />
      <circle cx="8" cy="8" r=".75" />
      <circle cx="12" cy="8" r=".75" />
    </>
  ),
  info: (
    <>
      <circle cx="8" cy="8" r="5.75" />
      <path d="M8 7.25v3.5M8 5.1v.15" />
    </>
  ),
  check: <path d="m3.75 8.25 2.75 2.75 5.75-6" />,
  tools: (
    <path d="M9.5 3.25a3 3 0 0 0-3.6 3.9L2.75 10.3a1.3 1.3 0 0 0 1.85 1.85l3.15-3.15a3 3 0 0 0 3.9-3.6L9.75 7.3 8.4 7.1l-.2-1.35z" />
  ),
}

export function Glyph({ name, className = 'size-3.5' }: { name: keyof typeof k_glyph | string; className?: string }) {
  return (
    <svg
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinecap="round"
      strokeLinejoin="round"
      className={cn('shrink-0', className)}
      aria-hidden="true"
    >
      {k_glyph[name]}
    </svg>
  )
}

/** a floating card under an anchor, drawn on the body so a panel's overflow does not clip it */
function Float({
  anchor,
  open,
  width,
  children,
}: {
  anchor: HTMLElement | null
  open: boolean
  width: number
  children: ReactNode
}) {
  const [at, setAt] = useState<{ left: number; top: number } | null>(null)
  useLayoutEffect(() => {
    if (!open || anchor === null) return
    const r = anchor.getBoundingClientRect()
    const left = Math.min(Math.max(8, r.left + r.width / 2 - width / 2), window.innerWidth - width - 8)
    setAt({ left, top: r.bottom + 6 })
  }, [open, anchor, width])
  if (!open || at === null) return null
  return createPortal(
    <div
      role="tooltip"
      style={{ left: at.left, top: at.top, width }}
      className="v3 v3-pop pointer-events-none fixed z-[100] rounded-md bg-raised px-3 py-2.5 text-xs leading-relaxed text-fg-mid shadow-[0_0_0_1px_var(--color-line),0_16px_40px_-10px_rgb(0_0_0/0.9)]"
    >
      {children}
    </div>,
    document.body,
  )
}

/** an (i) beside a title or a number that says what it is and why it matters, instead of small print on the screen */
export function Info({
  children,
  width = 280,
  className,
}: {
  children: ReactNode
  width?: number
  className?: string
}) {
  const [open, setOpen] = useState(false)
  const ref = useRef<HTMLButtonElement>(null)
  return (
    <>
      <button
        ref={ref}
        type="button"
        aria-label="What is this"
        onMouseEnter={() => setOpen(true)}
        onMouseLeave={() => setOpen(false)}
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        className={cn(
          'inline-grid size-4 shrink-0 cursor-help place-items-center rounded-full align-middle text-fg-dim transition-colors hover:text-fg focus-visible:text-fg',
          className,
        )}
      >
        <Glyph name="info" className="size-3.5" />
      </button>
      <Float anchor={ref.current} open={open} width={width}>
        {children}
      </Float>
    </>
  )
}

export interface PickItem<K extends string = string> {
  key: K
  label: ReactNode
  /** text the search box matches; the key when absent */
  text?: string
  /** a figure or a word at the right of the row */
  hint?: ReactNode
  /** a heading drawn above this row */
  group?: string
}

/** a button that drops a list to choose from, for anything longer than a handful of options; with `multi` the list
 *  stays open and every row toggles */
export function Picker<K extends string>({
  label,
  items,
  selected,
  onPick,
  multi = false,
  search = false,
  width = 260,
  align = 'left',
  footer,
  className,
}: {
  label: ReactNode
  items: readonly PickItem<K>[]
  selected: readonly K[]
  onPick: (key: K) => void
  multi?: boolean
  search?: boolean
  width?: number
  align?: 'left' | 'right'
  footer?: ReactNode
  className?: string
}) {
  const [open, setOpen] = useState(false)
  const [query, setQuery] = useState('')
  const box = useRef<HTMLDivElement>(null)
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
  useEffect(() => setQuery(''), [open])
  const q = query.trim().toLowerCase()
  const shown = q === '' ? items : items.filter((i) => (i.text ?? i.key).toLowerCase().includes(q))
  return (
    <div ref={box} className="relative">
      <button
        type="button"
        aria-expanded={open}
        onClick={() => setOpen((o) => !o)}
        className={cn(
          'flex h-8 cursor-pointer items-center gap-2 rounded-md bg-raised/60 px-3 text-sm text-fg transition-colors hover:bg-hover',
          open && 'bg-hover',
          className,
        )}
      >
        {label}
        <Glyph name="down" className={cn('size-3.5 text-fg-dim transition-transform', open && 'rotate-180')} />
      </button>
      {open && (
        <div
          style={{ width }}
          className={cn(
            'v3-pop absolute top-10 z-50 flex max-h-[min(28rem,70vh)] flex-col rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_20px_50px_-10px_rgb(0_0_0/0.9)]',
            align === 'left' ? 'left-0' : 'right-0',
          )}
        >
          {search && (
            <div className="flex h-9 shrink-0 items-center gap-2 border-b border-line px-2.5">
              <Glyph name="search" className="size-3.5 text-fg-dim" />
              <input
                autoFocus
                value={query}
                onChange={(e) => setQuery(e.target.value)}
                placeholder="Search"
                className="min-w-0 flex-1 bg-transparent text-sm text-fg outline-none placeholder:text-fg-dim"
              />
            </div>
          )}
          <div className="min-h-0 overflow-y-auto py-1">
            {shown.length === 0 && <div className="px-3 py-2 text-sm text-fg-dim">Nothing matches</div>}
            {shown.map((item, i) => {
              const on = selected.includes(item.key)
              return (
                <div key={item.key}>
                  {item.group !== undefined && item.group !== shown[i - 1]?.group && (
                    <div className="px-3 pt-2.5 pb-1 text-xs text-fg-dim">{item.group}</div>
                  )}
                  <button
                    type="button"
                    onClick={() => {
                      onPick(item.key)
                      if (!multi) setOpen(false)
                    }}
                    className={cn(
                      'flex h-8 w-full items-center gap-2.5 rounded-sm px-2.5 text-left text-sm transition-colors',
                      on ? 'bg-hover text-fg' : 'text-fg-mid hover:bg-hover hover:text-fg',
                    )}
                  >
                    {multi && (
                      <span
                        className={cn(
                          'grid size-4 shrink-0 place-items-center rounded-[4px] border',
                          on ? 'border-accent bg-accent/20 text-accent' : 'border-line',
                        )}
                      >
                        {on && <Glyph name="check" className="size-3" />}
                      </span>
                    )}
                    <span className="flex min-w-0 flex-1 items-center gap-2 truncate">{item.label}</span>
                    {item.hint !== undefined && <span className="mono shrink-0 text-xs text-fg-dim">{item.hint}</span>}
                  </button>
                </div>
              )
            })}
          </div>
          {footer !== undefined && (
            <div className="shrink-0 border-t border-line px-3 py-2 text-xs text-fg-dim">{footer}</div>
          )}
        </div>
      )}
    </div>
  )
}
