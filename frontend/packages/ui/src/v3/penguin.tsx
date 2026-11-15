import { useId, type CSSProperties } from 'react'
import { cn } from '../lib/format'

// what a page shows while it waits: a penguin in the brand's scarf sliding on its belly across the ice, frost behind
// it and snow over it. Drawn here, animated by the penguin-* rules in v3.css; it stands still under reduced motion

const k_snow = [
  [8, 0, 3.8],
  [21, 1.2, 4.6],
  [34, 0.4, 3.4],
  [47, 2.1, 5.0],
  [59, 0.9, 4.1],
  [72, 1.7, 3.6],
  [85, 0.2, 4.8],
  [94, 2.6, 4.3],
] as const

function Bird() {
  const id = useId()
  const back = `${id}-back`
  return (
    <svg viewBox="0 0 72 34" className="penguin-bird h-full w-auto overflow-visible" aria-hidden>
      <defs>
        <linearGradient id={back} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#34435c" />
          <stop offset="1" stopColor="#141b27" />
        </linearGradient>
      </defs>
      {/* flippers swept back, feet trailing */}
      <path
        d="M26 16c-9-1.5-16 .5-21 3.5 7 .5 14 .2 21-1.6z"
        fill="#1d2738"
        stroke="#38bdf8"
        strokeOpacity="0.35"
        strokeWidth="0.5"
      />
      <ellipse cx="7" cy="21.5" rx="3.2" ry="1.4" fill="#f59e0b" />
      <ellipse cx="6" cy="24" rx="3" ry="1.3" fill="#f59e0b" />
      <ellipse
        cx="31"
        cy="20"
        rx="23"
        ry="9"
        fill={`url(#${back})`}
        stroke="#38bdf8"
        strokeOpacity="0.45"
        strokeWidth="0.6"
      />
      <path
        d="M14 15.5c6-3.6 19-4.6 30-1.6"
        fill="none"
        stroke="#bfe9fd"
        strokeOpacity="0.35"
        strokeWidth="1"
        strokeLinecap="round"
      />
      <ellipse cx="31" cy="24.3" rx="18" ry="5.2" fill="#eef4fb" />
      <circle cx="54" cy="15" r="8.4" fill={`url(#${back})`} stroke="#38bdf8" strokeOpacity="0.45" strokeWidth="0.6" />
      <ellipse cx="56.3" cy="16.4" rx="4.8" ry="4.3" fill="#eef4fb" />
      <circle cx="57.6" cy="14.4" r="1.35" fill="#0a0a0c" />
      <circle cx="58" cy="14" r="0.45" fill="#fff" />
      <path d="M61.5 15.4l6.2 1.6-6.2 1.7z" fill="#f59e0b" />
      <circle cx="55.6" cy="19" r="1.3" fill="#38bdf8" opacity="0.35" />
      {/* the scarf, its tail flying */}
      <path d="M45.5 18.5c3 2.2 7.5 2.4 10.6.6l.4 2.6c-3.6 1.8-8.3 1.4-11.6-.8z" fill="#38bdf8" />
      <path className="penguin-scarf" d="M46.5 20.2c-4.5-.4-8.7.8-12 3.2 3.6.6 7.6.1 11.3-1.4z" fill="#38bdf8" />
    </svg>
  )
}

export function Penguin({ size = 'md', className }: { size?: 'sm' | 'md'; className?: string }) {
  const h = size === 'sm' ? 'h-16' : 'h-24'
  return (
    <div role="status" aria-label="loading" className={cn('relative w-full max-w-72 overflow-hidden', h, className)}>
      {k_snow.map(([left, delay, s], i) => (
        <span
          key={i}
          className="penguin-snow absolute top-0 size-[3px] rounded-full bg-white/50"
          style={{ left: `${left}%`, animationDelay: `${delay}s`, animationDuration: `${s}s` } as CSSProperties}
        />
      ))}
      <div className="absolute inset-x-0 bottom-[22%] h-px bg-gradient-to-r from-transparent via-[#38bdf8]/60 to-transparent" />
      <div className="absolute inset-x-6 bottom-[22%] h-3 translate-y-full bg-gradient-to-b from-[#38bdf8]/10 to-transparent blur-[2px]" />
      <div className="penguin-glint absolute bottom-[22%] h-px w-10 bg-gradient-to-r from-transparent via-white/80 to-transparent" />
      <div className="penguin-slide absolute bottom-[22%] flex h-[42%] items-end">
        <span className="penguin-trail mr-[-6px] mb-[2px] h-[3px] w-20 rounded-full bg-gradient-to-r from-transparent via-[#38bdf8]/40 to-white/90 shadow-[0_0_8px_rgb(56_189_248/0.6)]" />
        <Bird />
      </div>
      <div className="penguin-slide absolute bottom-[22%] flex h-[42%] origin-bottom -scale-y-100 items-end opacity-15 blur-[1px]">
        <span className="mr-[-6px] h-[3px] w-20" />
        <Bird />
      </div>
    </div>
  )
}

/** the penguin in the middle of whatever space it is given */
export function Waiting({ size, className }: { size?: 'sm' | 'md'; className?: string }) {
  return (
    <div className={cn('grid w-full place-items-center py-6', className)}>
      <Penguin {...(size === undefined ? {} : { size })} />
    </div>
  )
}
