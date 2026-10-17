import {
  Children,
  createContext,
  Fragment,
  isValidElement,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from 'react'
import { createPortal } from 'react-dom'
import { Panel, PanelGroup, PanelResizeHandle } from 'react-resizable-panels'
import { cn } from '../../lib/format'

/** the top bar's slot for a screen's own controls */
export const TopBarTools = createContext<HTMLElement | null>(null)

/** per viewer in localstorage; a failed read or write leaves the default arrangement */
const k_prefix = 'raikiri.layout.'
// react-resizable-panels saves a group's sizes under this prefix and its autoSaveId
const k_sizes_prefix = 'react-resizable-panels:'
const k_drag_type = 'application/x-raikiri-panel'

interface Layout {
  page: string
  generation: number
  panels: Record<string, ReactNode>
  /** which panel each slot shows; slots are named after the panel they start with */
  slots: Record<string, string>
  swap: (from: string, to: string) => void
}

const LayoutContext = createContext<Layout | null>(null)

function useLayout(): Layout {
  const layout = useContext(LayoutContext)
  if (layout === null) throw new Error('Split and Pane belong inside a Workspace')
  return layout
}

const sameSet = (a: string[], b: string[]) => [...a].sort().join() === [...b].sort().join()

function readSlots(page: string, ids: string[]): Record<string, string> {
  try {
    const saved = JSON.parse(localStorage.getItem(k_prefix + page) ?? 'null') as Record<
      string,
      string
    > | null
    // a panel added or dropped since the save makes the saved arrangement meaningless
    if (saved !== null && sameSet(Object.keys(saved), ids) && sameSet(Object.values(saved), ids)) {
      return saved
    }
  } catch {
    // unreadable storage starts from the default
  }
  return Object.fromEntries(ids.map((id) => [id, id]))
}

/** a screen of panels the viewer can resize, swap by dragging a header onto another, and put back */
export function Workspace({
  page,
  panels,
  children,
}: {
  page: string
  panels: Record<string, ReactNode>
  /** Split and Pane elements naming every panel once */
  children: ReactNode
}) {
  const ids = Object.keys(panels)
  const [slots, setSlots] = useState(() => readSlots(page, ids))
  const [generation, setGeneration] = useState(0)
  const tools = useContext(TopBarTools)

  useEffect(() => {
    try {
      localStorage.setItem(k_prefix + page, JSON.stringify(slots))
    } catch {
      // the arrangement then lasts for the session
    }
  }, [page, slots])

  const swap = useCallback((from: string, to: string) => {
    if (from === to) return
    setSlots((prev) => ({ ...prev, [from]: prev[to] ?? to, [to]: prev[from] ?? from }))
  }, [])

  const reset = () => {
    try {
      const sizes = `${k_sizes_prefix}${k_prefix}${page}.`
      for (const key of Object.keys(localStorage)) {
        if (key.startsWith(sizes)) localStorage.removeItem(key)
      }
    } catch {
      // nothing saved to clear
    }
    setSlots(Object.fromEntries(ids.map((id) => [id, id])))
    setGeneration((g) => g + 1)
  }

  return (
    <LayoutContext.Provider value={{ page, generation, panels, slots, swap }}>
      <div className="h-full min-h-0 min-w-0">{children}</div>
      {tools !== null &&
        createPortal(
          <button
            type="button"
            onClick={reset}
            title="Put every panel back at its first place and size"
            className="flex h-6 items-center rounded-sm px-2 text-2xs text-fg-dim transition-colors duration-150 hover:bg-hover hover:text-fg-mid"
          >
            Reset layout
          </button>,
          tools,
        )}
    </LayoutContext.Provider>
  )
}

/** panes side by side or stacked, with a grip between each pair */
export function Split({
  id,
  vertical = false,
  children,
}: {
  /** stable within the page; the sizes are saved under it */
  id: string
  vertical?: boolean
  children: ReactNode
}) {
  const { page, generation } = useLayout()
  const panes = Children.toArray(children).filter(isValidElement)
  return (
    <PanelGroup
      // a reset remounts the group so it starts again from the default sizes
      key={generation}
      direction={vertical ? 'vertical' : 'horizontal'}
      autoSaveId={`${k_prefix}${page}.${id}`}
    >
      {panes.map((pane, i) => (
        <Fragment key={pane.key ?? i}>
          {i > 0 && <Grip vertical={vertical} />}
          {pane}
        </Fragment>
      ))}
    </PanelGroup>
  )
}

/** a share of a Split, in percent; holds a slot's panel, or a nested Split */
export function Pane({
  slot,
  size,
  min,
  children,
}: {
  slot?: string
  size: number
  min?: number
  children?: ReactNode
}) {
  return (
    // visible so a dropdown near a panel's foot can reach over its neighbour
    <Panel defaultSize={size} minSize={min} style={{ overflow: 'visible' }}>
      {slot === undefined ? children : <Slot name={slot} />}
    </Panel>
  )
}

function Grip({ vertical }: { vertical: boolean }) {
  return (
    <PanelResizeHandle
      className={cn('group relative shrink-0 outline-none', vertical ? 'h-2' : 'w-2')}
    >
      <span
        className={cn(
          'absolute rounded-full bg-accent opacity-0 transition-opacity duration-150',
          'group-data-[resize-handle-state=hover]:opacity-50 group-data-[resize-handle-state=drag]:opacity-100',
          vertical ? 'inset-x-3 top-1/2 h-px -translate-y-1/2' : 'inset-y-3 left-1/2 w-px -translate-x-1/2',
        )}
      />
    </PanelResizeHandle>
  )
}

function Slot({ name }: { name: string }) {
  const { panels, slots, swap } = useLayout()
  const handle = useRef<HTMLElement | null>(null)
  const [armed, setArmed] = useState(false)
  const [lifted, setLifted] = useState(false)
  const [over, setOver] = useState(false)

  return (
    <div
      draggable={armed}
      onPointerDown={(e) => {
        const target = e.target as Element
        // a header's own buttons and inputs keep their clicks
        const grip = target.closest('button, input, select, textarea, a, label')
          ? null
          : target.closest<HTMLElement>('[data-drag-handle]')
        handle.current = grip
        setArmed(grip !== null)
      }}
      onPointerUp={() => setArmed(false)}
      onDragStart={(e) => {
        e.dataTransfer.setData(k_drag_type, name)
        e.dataTransfer.effectAllowed = 'move'
        if (handle.current !== null) e.dataTransfer.setDragImage(handle.current, 16, 12)
        setLifted(true)
      }}
      onDragEnd={() => {
        setArmed(false)
        setLifted(false)
      }}
      onDragOver={(e) => {
        if (!e.dataTransfer.types.includes(k_drag_type)) return
        e.preventDefault()
        e.dataTransfer.dropEffect = 'move'
        setOver(true)
      }}
      onDragLeave={(e) => {
        if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setOver(false)
      }}
      onDrop={(e) => {
        e.preventDefault()
        setOver(false)
        const from = e.dataTransfer.getData(k_drag_type)
        if (from !== '') swap(from, name)
      }}
      className={cn(
        'relative flex h-full min-h-0 min-w-0 flex-col *:min-h-0 *:flex-1',
        '[&_[data-drag-handle]]:cursor-grab [&_[data-drag-handle]]:select-none',
        'transition-opacity duration-150',
        lifted && 'opacity-40',
      )}
    >
      {panels[slots[name] ?? name]}
      {over && !lifted && (
        <div className="pointer-events-none absolute inset-0 z-40 rounded-md bg-accent/[0.05] shadow-[inset_0_0_0_1px_var(--color-accent)]" />
      )}
    </div>
  )
}
