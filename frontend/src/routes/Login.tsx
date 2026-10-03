import { lazy, Suspense, useEffect, useState } from 'react'
import { BrandMark } from '@/components/shared/BrandMark'
import { cn } from '@/lib/format'

/** lazy so the shader stays out of the main bundle */
const Lightning = lazy(() => import('@/components/backdrop/Lightning'))

/** hue of the brand cyan #38bdf8 */
const k_accent_hue = 199

const k_pending_ms = 900

export function Login() {
  const [pending, setPending] = useState(false)

  // regaining focus means the provider tab came back
  useEffect(() => {
    const onFocus = () => setPending(false)
    window.addEventListener('focus', onFocus)
    return () => window.removeEventListener('focus', onFocus)
  }, [])

  // not wired to a backend yet
  useEffect(() => {
    if (!pending) return
    const timer = setTimeout(() => setPending(false), k_pending_ms)
    return () => clearTimeout(timer)
  }, [pending])

  return (
    <div className="relative flex min-h-full items-center justify-center overflow-hidden px-6 py-12">
      <Backdrop />

      <div
        aria-hidden
        className="pointer-events-none absolute inset-0"
        style={{
          background:
            'radial-gradient(ellipse 30% 26% at 50% 50%, var(--color-canvas) 0%, color-mix(in srgb, var(--color-canvas) 78%, transparent) 45%, transparent 72%)',
        }}
      />

      <div className="relative flex w-full max-w-[352px] flex-col items-center">
        <BrandMark className="text-lg" />

        <div className="panel mt-6 w-full p-4">
          <SsoButton
            label="Continue with Google"
            loading={pending}
            disabled={pending}
            onClick={() => setPending(true)}
            icon={<GoogleIcon />}
          />
        </div>
      </div>
    </div>
  )
}

function SsoButton({
  label,
  icon,
  loading,
  disabled,
  onClick,
}: {
  label: string
  icon: React.ReactNode
  loading: boolean
  disabled: boolean
  onClick: () => void
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      className={cn(
        'relative flex h-9 w-full items-center justify-center gap-2.5 rounded-md bg-raised',
        'text-sm font-medium text-fg transition-[transform,background-color,opacity] duration-150',
        'hover:bg-hover active:scale-[0.99] disabled:opacity-50',
      )}
    >
      <span className="flex size-4 items-center justify-center">{loading ? <Spinner /> : icon}</span>
      <span>{label}</span>
    </button>
  )
}

function Spinner() {
  return (
    <svg viewBox="0 0 24 24" className="size-4 animate-spin" role="status" aria-label="Working">
      <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" strokeOpacity={0.25} strokeWidth={3} />
      <path
        d="M21 12a9 9 0 0 0-9-9"
        fill="none"
        stroke="currentColor"
        strokeWidth={3}
        strokeLinecap="round"
      />
    </svg>
  )
}

function Backdrop() {
  return (
    <div aria-hidden className="pointer-events-none absolute inset-0 overflow-hidden">
      <Suspense fallback={null}>
        <div className="absolute inset-0">
          {/* intensity above 1 clamps the core to white */}
          <Lightning
            hue={k_accent_hue}
            xOffset={0}
            speed={0.7}
            intensity={1.35}
            size={3}
            glow={0.11}
          />
        </div>
      </Suspense>
    </div>
  )
}

function GoogleIcon() {
  return (
    <svg viewBox="0 0 24 24" className="size-4" role="img" aria-label="Google">
      <path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z" />
      <path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z" />
      <path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z" />
      <path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z" />
    </svg>
  )
}

