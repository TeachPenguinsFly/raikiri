import { useRef, useState } from 'react'
import { Venue } from '@/components/shared/Venue'
import { useDismiss } from '@/lib/dismiss'
import { cn } from '@/lib/format'

/** venues come from the data; an empty selection means all */
export function VenueFilter({
  available,
  selected,
  onChange,
}: {
  available: string[]
  selected: Set<string>
  onChange: (next: Set<string>) => void
}) {
  const [open, setOpen] = useState(false)
  const box = useRef<HTMLDivElement>(null)
  useDismiss(box, open, setOpen)

  const toggle = (venue: string) => {
    const next = new Set(selected)
    if (next.has(venue)) next.delete(venue)
    else next.add(venue)
    onChange(next)
  }

  const label =
    selected.size === 0 ? `All ${available.length} venues` : `${selected.size} of ${available.length}`

  return (
    <div ref={box} className="relative">
      <button
        type="button"
        onClick={() => setOpen((was) => !was)}
        className={cn(
          'flex h-6 items-center gap-1.5 rounded border border-line px-2 text-2xs transition-colors duration-150',
          open ? 'border-accent text-fg' : 'text-fg-dim hover:text-fg-mid',
        )}
      >
        {label}
        <span className="text-[8px] leading-none">▾</span>
      </button>

      {open && (
        <div className="absolute right-0 z-20 mt-1 max-h-80 w-52 overflow-auto rounded border border-line bg-raised py-1 shadow-lg">
          <button
            type="button"
            onClick={() => onChange(new Set())}
            className="w-full px-2 py-1 text-left text-2xs text-fg-dim hover:bg-hover hover:text-fg-mid"
          >
            Clear — show all
          </button>
          <div className="my-1 border-t border-line/60" />
          {available.map((venue) => (
            <label
              key={venue}
              className="flex cursor-pointer items-center gap-2 px-2 py-1 text-xs hover:bg-hover"
            >
              <input
                type="checkbox"
                checked={selected.has(venue)}
                onChange={() => toggle(venue)}
                className="size-3 accent-accent"
              />
              <Venue venue={venue} />
            </label>
          ))}
        </div>
      )}
    </div>
  )
}
