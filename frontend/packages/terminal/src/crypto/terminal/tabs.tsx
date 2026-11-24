import { cn } from '@raikiri/ui'

/** a block's top line: text tabs, the open one underlined */
export function Tabs<T extends string>({
  tabs,
  value,
  onChange,
}: {
  tabs: ReadonlyArray<readonly [T, string]>
  value: T
  onChange: (tab: T) => void
}) {
  return (
    <div className="flex h-9 shrink-0 items-stretch gap-4 border-b border-line px-3" role="tablist">
      {tabs.map(([id, label]) => (
        <button
          key={id}
          type="button"
          role="tab"
          aria-selected={value === id}
          onClick={() => onChange(id)}
          className={cn(
            'relative cursor-pointer text-xs font-medium whitespace-nowrap transition-colors',
            value === id ? 'text-fg' : 'text-fg-dim hover:text-fg-mid',
          )}
        >
          {label}
          {value === id && <span className="absolute inset-x-0 -bottom-px h-0.5 bg-accent" />}
        </button>
      ))}
    </div>
  )
}
