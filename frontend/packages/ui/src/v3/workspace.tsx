import { useCallback, useEffect, useRef, useState, type ReactNode } from 'react'
import { cn } from '../lib/format'
import { AddMenu, Dock, openTab, tabsOf, type DockNode, type Widget } from './dock'
import { Glyph, Seg } from './ui'

// a terminal's screen: the dock with its presets, kept per viewer in the browser, and the bar under it; a narrow
// screen stacks the widgets instead

export interface Preset {
  label: string
  build: () => DockNode
}

function valid(node: unknown, widgets: Record<string, Widget>): node is DockNode {
  if (typeof node !== 'object' || node === null) return false
  const n = node as DockNode
  if (n.type === 'group')
    return Array.isArray(n.tabs) && n.tabs.length > 0 && n.tabs.every((t) => t in widgets) && typeof n.id === 'string'
  return (
    n.type === 'split' &&
    Array.isArray(n.children) &&
    n.children.length > 0 &&
    n.children.every((c) => valid(c, widgets)) &&
    Array.isArray(n.sizes)
  )
}

function load<P extends string>(
  key: string,
  presets: Record<P, Preset>,
  fallback: P,
  widgets: Record<string, Widget>,
): { preset: P; layout: DockNode } {
  try {
    const saved = JSON.parse(localStorage.getItem(key) ?? 'null') as { preset?: P; layout?: unknown } | null
    if (saved !== null && valid(saved.layout, widgets) && saved.preset !== undefined && saved.preset in presets) {
      const tabs = tabsOf(saved.layout)
      if (new Set(tabs).size === tabs.length) return { preset: saved.preset, layout: saved.layout }
    }
  } catch {
    // an unreadable save starts from the default
  }
  return { preset: fallback, layout: presets[fallback].build() }
}

function save(key: string, preset: string, layout: DockNode) {
  try {
    localStorage.setItem(key, JSON.stringify({ preset, layout }))
  } catch {
    // the arrangement lasts for this page only
  }
}

export function useNarrow(): boolean {
  const q = '(max-width: 899px)'
  const [narrow, setNarrow] = useState(() => window.matchMedia(q).matches)
  useEffect(() => {
    const m = window.matchMedia(q)
    const on = () => setNarrow(m.matches)
    m.addEventListener('change', on)
    return () => m.removeEventListener('change', on)
  }, [])
  return narrow
}

function StatusBar<P extends string>({
  presets,
  preset,
  onPreset,
  onAdd,
  open,
  widgets,
  connected,
  clock,
}: {
  presets: Record<P, Preset>
  preset: P
  onPreset: (p: P) => void
  onAdd: (w: string) => void
  open: string[]
  widgets: Record<string, Widget>
  connected: boolean
  clock?: ReactNode
}) {
  const [menu, setMenu] = useState(false)
  const close = useCallback(() => setMenu(false), [])
  return (
    <footer className="flex h-9 shrink-0 items-center gap-2 border-t border-line px-3 text-2xs text-fg-dim">
      <span className={cn('flex items-center gap-1.5', connected ? 'text-up' : 'text-amber')}>
        <span className={cn('size-1.5 rounded-full', connected ? 'pulse-dot bg-up' : 'bg-amber')} />
        {connected ? 'Live stream' : 'Reconnecting'}
      </span>
      <span className="h-4 w-px bg-line" />
      {Object.keys(presets).length > 1 && (
        <Seg
          size="xs"
          value={preset}
          onChange={onPreset}
          options={(Object.entries(presets) as Array<[P, Preset]>).map(([p, { label }]) => [p, label] as const)}
        />
      )}
      <div className="relative">
        <button
          type="button"
          onClick={() => setMenu((m) => !m)}
          className="flex h-6 cursor-pointer items-center gap-1 rounded-md px-2 text-fg-mid transition-colors hover:bg-raised hover:text-fg"
        >
          <Glyph name="plus" className="size-3" />
          Widget
        </button>
        {menu && (
          <AddMenu
            up
            items={Object.entries(widgets)
              .filter(([id]) => !open.includes(id))
              .map(([id, w]) => [id, w.title] as const)}
            onPick={(w) => {
              setMenu(false)
              onAdd(w)
            }}
            onClose={close}
          />
        )}
      </div>
      <span className="flex-1" />
      {clock !== undefined && (
        <>
          <span className="h-4 w-px bg-line" />
          {clock}
        </>
      )}
    </footer>
  )
}

/** the first widget short on top, each of the rest full height under it with its title */
function Stacked({ widgets, order }: { widgets: Record<string, Widget>; order: readonly string[] }) {
  const [lead, ...rest] = order
  return (
    <div className="flex h-full flex-col gap-2 overflow-y-auto p-2">
      <section className="h-64 shrink-0 overflow-hidden rounded-lg bg-panel shadow-[0_0_0_1px_var(--color-line)]">
        {lead === undefined ? null : widgets[lead]?.render()}
      </section>
      {rest.map((id) => (
        <section
          key={id}
          className="flex h-[460px] shrink-0 flex-col overflow-hidden rounded-lg bg-panel shadow-[0_0_0_1px_var(--color-line)]"
        >
          <div className="flex h-8 shrink-0 items-center gap-2 border-b border-line px-3 text-xs text-fg-mid">
            {widgets[id]?.title}
            <span className="ml-auto">{widgets[id]?.tools?.()}</span>
          </div>
          <div className="min-h-0 flex-1">{widgets[id]?.render()}</div>
        </section>
      ))}
    </div>
  )
}

export function Workspace<P extends string>({
  widgets,
  presets,
  fallback,
  storageKey,
  connected,
  clock,
  stacked,
}: {
  widgets: Record<string, Widget>
  presets: Record<P, Preset>
  fallback: P
  storageKey: string
  connected: boolean
  clock?: ReactNode
  /** the widgets a narrow screen shows, in order */
  stacked: readonly string[]
}) {
  const first = useRef<{ preset: P; layout: DockNode } | null>(null)
  first.current ??= load(storageKey, presets, fallback, widgets)
  const [preset, setPreset] = useState<P>(first.current.preset)
  const [layout, setLayout] = useState<DockNode>(first.current.layout)
  const narrow = useNarrow()
  const change = useCallback(
    (next: DockNode) => {
      setLayout(next)
      save(storageKey, preset, next)
    },
    [preset, storageKey],
  )
  const resize = useCallback((next: DockNode) => save(storageKey, preset, next), [preset, storageKey])
  const applyPreset = (p: P) => {
    const next = presets[p].build()
    setPreset(p)
    setLayout(next)
    save(storageKey, p, next)
  }
  if (narrow) return <Stacked widgets={widgets} order={stacked} />
  return (
    <div className="flex h-full min-h-0 flex-col">
      <div className="min-h-0 flex-1 p-1.5">
        {/* a new preset is a new tree: the key remounts the panel groups at its sizes */}
        <Dock key={`${preset}:${layout.id}`} layout={layout} onChange={change} onResize={resize} widgets={widgets} />
      </div>
      <StatusBar
        presets={presets}
        preset={preset}
        onPreset={applyPreset}
        onAdd={(w) => change(openTab(layout, w))}
        open={tabsOf(layout)}
        widgets={widgets}
        connected={connected}
        clock={clock}
      />
    </div>
  )
}
