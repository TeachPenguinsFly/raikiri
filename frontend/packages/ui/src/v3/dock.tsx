import { Fragment, useCallback, useEffect, useRef, useState, type DragEvent, type ReactNode } from 'react'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { cn } from '../lib/format'
import { Glyph, trackPointer } from './ui'

// a screen of widgets in tab groups: drag a tab onto another group to join it or onto a group's edge to split it,
// drag the gaps to resize, double-click a tab to fill the screen with its group. The arrangement is one tree, kept
// per viewer in the browser

export type DockNode =
  | { type: 'split'; id: string; dir: 'row' | 'col'; children: DockNode[]; sizes: number[] }
  | { type: 'group'; id: string; tabs: string[]; active: string }

export interface Widget {
  title: string
  icon?: string
  render: () => ReactNode
  /** controls in the tab bar's right end while the widget is in front */
  tools?: () => ReactNode
}

type Zone = 'center' | 'left' | 'right' | 'top' | 'bottom'

const k_drag = 'application/x-raikiri-tab'
let k_seq = 0
const uid = (p: string) => `${p}${Date.now().toString(36)}${(k_seq++).toString(36)}`

export const group = (tabs: string[], id = uid('g')): DockNode => ({ type: 'group', id, tabs, active: tabs[0] ?? '' })
export const split = (dir: 'row' | 'col', children: DockNode[], sizes: number[], id = uid('s')): DockNode => ({
  type: 'split',
  id,
  dir,
  children,
  sizes,
})

export function tabsOf(node: DockNode): string[] {
  return node.type === 'group' ? node.tabs : node.children.flatMap(tabsOf)
}

function groupOf(node: DockNode, tab: string): string | undefined {
  if (node.type === 'group') return node.tabs.includes(tab) ? node.id : undefined
  for (const c of node.children) {
    const g = groupOf(c, tab)
    if (g !== undefined) return g
  }
  return undefined
}

/** a split of one keeps only its child, and a child split the same way as its parent gives the parent its children */
function prune(node: DockNode): DockNode | null {
  if (node.type === 'group') return node.tabs.length === 0 ? null : node
  const children: DockNode[] = []
  const sizes: number[] = []
  node.children.forEach((c, i) => {
    const p = prune(c)
    if (p === null) return
    const share = node.sizes[i] ?? 100 / node.children.length
    if (p.type === 'split' && p.dir === node.dir) {
      const total = p.sizes.reduce((a, b) => a + b, 0) || 1
      p.children.forEach((cc, j) => {
        children.push(cc)
        sizes.push((share * (p.sizes[j] ?? 0)) / total)
      })
    } else {
      children.push(p)
      sizes.push(share)
    }
  })
  if (children.length === 0) return null
  if (children.length === 1) return children[0] ?? null
  const total = sizes.reduce((a, b) => a + b, 0) || 1
  return { ...node, children, sizes: sizes.map((s) => (s / total) * 100) }
}

function removeTab(node: DockNode, tab: string): DockNode {
  if (node.type === 'group') {
    if (!node.tabs.includes(tab)) return node
    const tabs = node.tabs.filter((t) => t !== tab)
    const at = node.tabs.indexOf(tab)
    return { ...node, tabs, active: node.active === tab ? (tabs[Math.max(0, at - 1)] ?? '') : node.active }
  }
  return { ...node, children: node.children.map((c) => removeTab(c, tab)) }
}

const dirOf = (zone: Zone) => (zone === 'left' || zone === 'right' ? 'row' : 'col')
const before = (zone: Zone) => zone === 'left' || zone === 'top'

function insertTab(node: DockNode, target: string, tab: string, zone: Zone, index?: number): DockNode {
  if (node.type === 'group') {
    if (node.id !== target) return node
    if (zone === 'center') {
      const tabs = [...node.tabs]
      tabs.splice(index ?? tabs.length, 0, tab)
      return { ...node, tabs, active: tab }
    }
    const fresh = group([tab])
    return split(dirOf(zone), before(zone) ? [fresh, node] : [node, fresh], [50, 50])
  }
  const at = node.children.findIndex((c) => c.type === 'group' && c.id === target)
  if (at >= 0 && zone !== 'center' && dirOf(zone) === node.dir) {
    const children = [...node.children]
    const sizes = [...node.sizes]
    const half = (sizes[at] ?? 50) / 2
    sizes[at] = half
    const i = before(zone) ? at : at + 1
    children.splice(i, 0, group([tab]))
    sizes.splice(i, 0, half)
    return { ...node, children, sizes }
  }
  return { ...node, children: node.children.map((c) => insertTab(c, target, tab, zone, index)) }
}

export function moveTab(root: DockNode, tab: string, target: string, zone: Zone, index?: number): DockNode {
  const from = groupOf(root, tab)
  if (from === target && zone === 'center' && index === undefined) return root
  if (from === target && zone !== 'center') {
    const g = findGroup(root, target)
    if (g !== undefined && g.tabs.length === 1) return root
  }
  let next = root
  if (from !== undefined) {
    // a tab moved inside its own bar keeps its place in the count
    const g = findGroup(root, from)
    const shift = from === target && index !== undefined && g !== undefined && g.tabs.indexOf(tab) < index ? 1 : 0
    next = removeTab(next, tab)
    if (index !== undefined) index -= shift
  }
  next = insertTab(next, target, tab, zone, index)
  return prune(next) ?? group([tab])
}

export function closeTab(root: DockNode, tab: string): DockNode | null {
  return prune(removeTab(root, tab))
}

/** a widget not on screen joins the group holding `near`, or the biggest group */
export function openTab(root: DockNode, tab: string, near?: string): DockNode {
  const existing = groupOf(root, tab)
  if (existing !== undefined) return activate(root, existing, tab)
  const target = (near !== undefined ? groupOf(root, near) : undefined) ?? firstGroup(root).id
  return insertTab(root, target, tab, 'center')
}

function firstGroup(node: DockNode): Extract<DockNode, { type: 'group' }> {
  return node.type === 'group' ? node : firstGroup(node.children[0] as DockNode)
}

function findGroup(node: DockNode, id: string): Extract<DockNode, { type: 'group' }> | undefined {
  if (node.type === 'group') return node.id === id ? node : undefined
  for (const c of node.children) {
    const g = findGroup(c, id)
    if (g !== undefined) return g
  }
  return undefined
}

function activate(node: DockNode, id: string, tab: string): DockNode {
  if (node.type === 'group') return node.id === id ? { ...node, active: tab } : node
  return { ...node, children: node.children.map((c) => activate(c, id, tab)) }
}

/** the sizes the viewer dragged to, written into the tree so the next change starts from them */
function bake(node: DockNode, sizes: Map<string, number[]>): DockNode {
  if (node.type === 'group') return node
  const s = sizes.get(node.id)
  return {
    ...node,
    sizes: s !== undefined && s.length === node.children.length ? s : node.sizes,
    children: node.children.map((c) => bake(c, sizes)),
  }
}

function zoneAt(e: DragEvent<HTMLElement>): Zone {
  const r = e.currentTarget.getBoundingClientRect()
  const x = (e.clientX - r.left) / r.width
  const y = (e.clientY - r.top) / r.height
  const edge = Math.min(x, 1 - x, y, 1 - y)
  if (edge > 0.22) return 'center'
  if (edge === x) return 'left'
  if (edge === 1 - x) return 'right'
  return edge === y ? 'top' : 'bottom'
}

const k_zone_box: Record<Zone, string> = {
  center: 'inset-2',
  left: 'inset-y-2 left-2 w-[calc(50%-12px)]',
  right: 'inset-y-2 right-2 w-[calc(50%-12px)]',
  top: 'inset-x-2 top-2 h-[calc(50%-12px)]',
  bottom: 'inset-x-2 bottom-2 h-[calc(50%-12px)]',
}

export function Dock({
  layout,
  onChange,
  onResize,
  widgets,
}: {
  layout: DockNode
  /** a tab moved, opened or closed: the tree is redrawn */
  onChange: (next: DockNode) => void
  /** a gap was dragged: the tree is only kept, since the panels already show the sizes */
  onResize: (next: DockNode) => void
  widgets: Record<string, Widget>
}) {
  const [dragging, setDragging] = useState<string | null>(null)
  const [maxed, setMaxed] = useState<string | null>(null)
  const sizes = useRef(new Map<string, number[]>())
  const change = useCallback((next: DockNode) => onChange(next), [onChange])

  useEffect(() => {
    const end = () => setDragging(null)
    window.addEventListener('dragend', end)
    window.addEventListener('drop', end)
    return () => {
      window.removeEventListener('dragend', end)
      window.removeEventListener('drop', end)
    }
  }, [])
  useEffect(() => {
    if (maxed === null) return
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && setMaxed(null)
    window.addEventListener('keydown', esc)
    return () => window.removeEventListener('keydown', esc)
  }, [maxed])

  const apply = (op: (root: DockNode) => DockNode | null) => {
    const next = op(bake(layout, sizes.current))
    if (next !== null) change(next)
  }
  const onLayout = (id: string, s: number[]) => {
    sizes.current.set(id, s)
    onResize(bake(layout, sizes.current))
  }

  const render = (node: DockNode): ReactNode => {
    if (node.type === 'split') {
      return (
        <PanelGroup
          direction={node.dir === 'row' ? 'horizontal' : 'vertical'}
          id={node.id}
          onLayout={(s) => onLayout(node.id, s)}
        >
          {node.children.map((c, i) => (
            <Fragment key={c.id}>
              {i > 0 && (
                <PanelResizeHandle
                  className={cn(
                    'group relative shrink-0 outline-none',
                    node.dir === 'row' ? 'w-1.5 cursor-col-resize' : 'h-1.5 cursor-row-resize',
                  )}
                >
                  <span
                    className={cn(
                      'absolute rounded-full bg-accent/0 transition-colors duration-200 group-hover:bg-accent/60 group-data-[resize-handle-state=drag]:bg-accent',
                      node.dir === 'row'
                        ? 'inset-y-6 left-1/2 w-px -translate-x-1/2'
                        : 'inset-x-6 top-1/2 h-px -translate-y-1/2',
                    )}
                  />
                </PanelResizeHandle>
              )}
              <Panel id={c.id} order={i} defaultSize={node.sizes[i] ?? 100 / node.children.length} minSize={6}>
                {render(c)}
              </Panel>
            </Fragment>
          ))}
        </PanelGroup>
      )
    }
    return (
      <GroupView
        node={node}
        widgets={widgets}
        dragging={dragging}
        onDragStart={setDragging}
        onActivate={(tab) => apply((r) => activate(r, node.id, tab))}
        onClose={(tab) => apply((r) => closeTab(r, tab))}
        onDrop={(tab, zone, index) => apply((r) => moveTab(r, tab, node.id, zone, index))}
        onAdd={(tab) => apply((r) => insertTab(r, node.id, tab, 'center'))}
        missing={Object.keys(widgets).filter((w) => !tabsOf(layout).includes(w))}
        maxed={maxed === node.id}
        onMax={() => setMaxed((m) => (m === node.id ? null : node.id))}
      />
    )
  }

  const max = maxed === null ? undefined : findGroup(layout, maxed)
  return (
    <div className="relative h-full min-h-0">
      {render(layout)}
      {max !== undefined && (
        <div className="v3-pop absolute inset-0 z-30 rounded-lg bg-canvas shadow-[0_0_0_1px_var(--color-line),0_30px_80px_-20px_rgb(0_0_0/0.9)]">
          <GroupView
            node={max}
            widgets={widgets}
            dragging={null}
            onDragStart={() => undefined}
            onActivate={(tab) => apply((r) => activate(r, max.id, tab))}
            onClose={(tab) => {
              setMaxed(null)
              apply((r) => closeTab(r, tab))
            }}
            onDrop={() => undefined}
            onAdd={(tab) => apply((r) => insertTab(r, max.id, tab, 'center'))}
            missing={[]}
            maxed
            onMax={() => setMaxed(null)}
          />
        </div>
      )}
    </div>
  )
}

function GroupView({
  node,
  widgets,
  dragging,
  onDragStart,
  onActivate,
  onClose,
  onDrop,
  onAdd,
  missing,
  maxed,
  onMax,
}: {
  node: Extract<DockNode, { type: 'group' }>
  widgets: Record<string, Widget>
  dragging: string | null
  onDragStart: (tab: string) => void
  onActivate: (tab: string) => void
  onClose: (tab: string) => void
  onDrop: (tab: string, zone: Zone, index?: number) => void
  onAdd: (tab: string) => void
  missing: string[]
  maxed: boolean
  onMax: () => void
}) {
  const [zone, setZone] = useState<Zone | null>(null)
  const [mark, setMark] = useState<number | null>(null)
  const [menu, setMenu] = useState(false)
  const [ctx, setCtx] = useState<{ tab: string; x: number; y: number } | null>(null)
  const active = widgets[node.active]
  const tabOf = (e: DragEvent) => e.dataTransfer.getData(k_drag) || dragging

  return (
    <section
      className="v3-glow flex h-full min-h-0 flex-col overflow-hidden rounded-lg bg-panel shadow-[0_0_0_1px_var(--color-line)]"
      onPointerMove={trackPointer}
    >
      <div
        className="relative flex h-8 shrink-0 items-center gap-0.5 border-b border-line pr-1 pl-1"
        onDragOver={(e) => {
          if (dragging === null) return
          e.preventDefault()
          const tabs = [...e.currentTarget.querySelectorAll<HTMLElement>('[data-tab]')]
          const i = tabs.findIndex((t) => {
            const r = t.getBoundingClientRect()
            return e.clientX < r.left + r.width / 2
          })
          setMark(i < 0 ? tabs.length : i)
        }}
        onDragLeave={() => setMark(null)}
        onDrop={(e) => {
          e.preventDefault()
          const tab = tabOf(e)
          if (tab !== null && tab !== '') onDrop(tab, 'center', mark ?? undefined)
          setMark(null)
        }}
      >
        <div className="flex min-w-0 flex-1 items-center gap-0.5 overflow-x-auto [scrollbar-width:none]">
          {node.tabs.map((t, i) => {
            const w = widgets[t]
            const on = t === node.active
            return (
              <div key={t} className="relative flex items-center">
                {mark === i && <span className="absolute inset-y-1 -left-0.5 w-0.5 rounded-full bg-accent" />}
                <button
                  type="button"
                  data-tab={t}
                  draggable={!maxed}
                  onDragStart={(e) => {
                    e.dataTransfer.setData(k_drag, t)
                    e.dataTransfer.effectAllowed = 'move'
                    onDragStart(t)
                  }}
                  onClick={() => onActivate(t)}
                  onDoubleClick={onMax}
                  onContextMenu={(e) => {
                    e.preventDefault()
                    setCtx({ tab: t, x: e.clientX, y: e.clientY })
                  }}
                  className={cn(
                    'group/tab relative flex h-6 shrink-0 cursor-grab items-center gap-1.5 rounded-md pr-1 pl-2 text-xs transition-colors duration-150 active:cursor-grabbing',
                    on ? 'bg-raised text-fg' : 'text-fg-dim hover:bg-raised/50 hover:text-fg-mid',
                    dragging === t && 'opacity-40',
                  )}
                >
                  {w?.icon !== undefined && <Glyph name={w.icon} className={cn('size-3', on ? 'text-accent' : '')} />}
                  {w?.title ?? t}
                  <span
                    role="button"
                    tabIndex={-1}
                    aria-label={`close ${w?.title ?? t}`}
                    onClick={(e) => {
                      e.stopPropagation()
                      onClose(t)
                    }}
                    className="grid size-4 place-items-center rounded-sm text-fg-dim opacity-0 transition-opacity group-hover/tab:opacity-100 hover:bg-hover hover:text-fg"
                  >
                    <Glyph name="close" className="size-2.5" />
                  </span>
                </button>
              </div>
            )
          })}
          {mark === node.tabs.length && <span className="h-4 w-0.5 rounded-full bg-accent" />}
          {missing.length > 0 && (
            <div className="relative">
              <button
                type="button"
                aria-label="add a widget"
                onClick={() => setMenu((m) => !m)}
                className="grid size-6 place-items-center rounded-md text-fg-dim transition-colors hover:bg-raised hover:text-fg"
              >
                <Glyph name="plus" className="size-3" />
              </button>
              {menu && (
                <AddMenu
                  items={missing.map((m) => [m, widgets[m]?.title ?? m] as const)}
                  onPick={(m) => {
                    setMenu(false)
                    onAdd(m)
                  }}
                  onClose={() => setMenu(false)}
                />
              )}
            </div>
          )}
        </div>
        {active?.tools?.()}
        <button
          type="button"
          aria-label={maxed ? 'restore' : 'fill the screen'}
          onClick={onMax}
          className="grid size-6 shrink-0 place-items-center rounded-md text-fg-dim transition-colors hover:bg-raised hover:text-fg"
        >
          <Glyph name={maxed ? 'min' : 'max'} className="size-3" />
        </button>
      </div>
      {ctx !== null && (
        <TabMenu
          at={ctx}
          single={node.tabs.length === 1}
          onClose={() => setCtx(null)}
          onSplit={(zone) => onDrop(ctx.tab, zone)}
          onMax={onMax}
          onRemove={() => onClose(ctx.tab)}
        />
      )}
      <div className="relative min-h-0 flex-1">
        {active === undefined ? null : (
          <div key={node.active} className="h-full min-h-0 animate-[enter-fade_0.2s_ease-out]">
            {active.render()}
          </div>
        )}
        {dragging !== null && (
          <div
            className="absolute inset-0 z-20"
            onDragOver={(e) => {
              e.preventDefault()
              setZone(zoneAt(e))
            }}
            onDragLeave={() => setZone(null)}
            onDrop={(e) => {
              e.preventDefault()
              const tab = tabOf(e)
              if (tab !== null && tab !== '') onDrop(tab, zoneAt(e))
              setZone(null)
            }}
          >
            {zone !== null && (
              <div
                className={cn(
                  'absolute rounded-md border border-accent/60 bg-accent/10 shadow-[0_0_40px_-8px_rgb(var(--v3-glow)/0.6)_inset] transition-all duration-200 ease-[cubic-bezier(0.22,1,0.36,1)]',
                  k_zone_box[zone],
                )}
              />
            )}
          </div>
        )}
      </div>
    </section>
  )
}

export function AddMenu({
  items,
  onPick,
  onClose,
  up = false,
}: {
  items: ReadonlyArray<readonly [string, string]>
  onPick: (id: string) => void
  onClose: () => void
  up?: boolean
}) {
  const box = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const off = (e: MouseEvent) => {
      if (box.current !== null && !box.current.contains(e.target as Node)) onClose()
    }
    window.addEventListener('mousedown', off)
    return () => window.removeEventListener('mousedown', off)
  }, [onClose])
  return (
    <div
      ref={box}
      className={cn(
        'v3-pop absolute left-0 z-40 min-w-40 rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_16px_40px_-8px_rgb(0_0_0/0.8)]',
        up ? 'bottom-8' : 'top-7',
      )}
    >
      {items.length === 0 ? (
        <div className="px-2 py-1.5 text-2xs text-fg-dim">every widget is open</div>
      ) : (
        items.map(([id, label]) => (
          <button
            key={id}
            type="button"
            onClick={() => onPick(id)}
            className="flex h-7 w-full items-center gap-2 rounded-sm px-2 text-left text-xs text-fg-mid transition-colors hover:bg-hover hover:text-fg"
          >
            <Glyph name="plus" className="size-3 text-fg-dim" />
            {label}
          </button>
        ))
      )}
    </div>
  )
}

/** the same moves a drag makes, a click each, for a trackpad or a keyboard */
function TabMenu({
  at,
  single,
  onClose,
  onSplit,
  onMax,
  onRemove,
}: {
  at: { x: number; y: number }
  single: boolean
  onClose: () => void
  onSplit: (zone: Zone) => void
  onMax: () => void
  onRemove: () => void
}) {
  const box = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const off = (e: MouseEvent) => {
      if (box.current !== null && !box.current.contains(e.target as Node)) onClose()
    }
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && onClose()
    window.addEventListener('mousedown', off)
    window.addEventListener('keydown', esc)
    box.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus()
    return () => {
      window.removeEventListener('mousedown', off)
      window.removeEventListener('keydown', esc)
    }
  }, [onClose])
  const items: Array<[string, () => void, boolean]> = [
    ['Split right', () => onSplit('right'), single],
    ['Split down', () => onSplit('bottom'), single],
    ['Fill the screen', onMax, false],
    ['Close', onRemove, false],
  ]
  return (
    <div
      ref={box}
      role="menu"
      className="v3-pop fixed z-50 min-w-40 rounded-md bg-raised p-1 shadow-[0_0_0_1px_var(--color-line),0_16px_40px_-8px_rgb(0_0_0/0.8)]"
      style={{ left: at.x, top: at.y }}
    >
      {items.map(([label, run, off]) => (
        <button
          key={label}
          type="button"
          role="menuitem"
          disabled={off}
          onClick={() => {
            onClose()
            run()
          }}
          className="flex h-7 w-full cursor-pointer items-center rounded-sm px-2 text-left text-xs text-fg-mid transition-colors hover:bg-hover hover:text-fg focus-visible:bg-hover disabled:cursor-default disabled:opacity-35 disabled:hover:bg-transparent"
        >
          {label}
        </button>
      ))}
    </div>
  )
}
