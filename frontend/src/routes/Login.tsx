import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router'
import { BrandLockup } from '@/components/shared/BrandMark'
import { cn } from '@/lib/utils'

type Provider = 'google' | 'github'

const LAST_USED = 'raikiri.last-provider'

/** Deliberately not a full RFC 5322 parser. The address is checked by the
 *  service that mails it; this only catches the typo before a round trip. */
function looksLikeEmail(value: string): boolean {
  return /^[^\s@]+@[^\s@]+\.[^\s@]{2,}$/.test(value.trim())
}

export function Login() {
  const navigate = useNavigate()
  const [email, setEmail] = useState('')
  const [invalid, setInvalid] = useState<string | null>(null)
  const [pending, setPending] = useState<Provider | 'email' | null>(null)
  const [notice, setNotice] = useState<string | null>(null)
  const [lastUsed, setLastUsed] = useState<Provider | null>(null)

  useEffect(() => {
    const stored = localStorage.getItem(LAST_USED)
    if (stored === 'google' || stored === 'github') setLastUsed(stored)
  }, [])

  // A provider tab that never returns would leave the button spinning for the
  // rest of the session; coming back to the window is the signal that it did.
  useEffect(() => {
    const onFocus = () => setPending(null)
    window.addEventListener('focus', onFocus)
    return () => window.removeEventListener('focus', onFocus)
  }, [])

  const startSso = (provider: Provider) => {
    setPending(provider)
    setNotice(null)
    localStorage.setItem(LAST_USED, provider)
    setLastUsed(provider)
    unavailable(setPending, setNotice)
  }

  const submitEmail = (event: React.FormEvent) => {
    event.preventDefault()
    if (!looksLikeEmail(email)) {
      setInvalid('That is not an address this will reach.')
      return
    }
    setInvalid(null)
    setPending('email')
    setNotice(null)
    unavailable(setPending, setNotice)
  }

  const busy = pending !== null

  return (
    <div className="relative flex min-h-full items-center justify-center overflow-hidden px-6 py-12">
      <Backdrop />

      <div className="relative flex w-full max-w-sm flex-col items-center">
        <BrandLockup />
        <p className="mt-5 font-mono text-sm tracking-tight text-fg-mid">raikiri</p>
        <p className="mt-1 text-sm text-fg-dim">Sign in to the trading terminal.</p>

        <div className="mt-7 w-full rounded-[5px] border border-line bg-panel/70 p-6 shadow-[0_1px_0_0_rgba(255,255,255,0.04)_inset,0_40px_80px_-40px_rgba(0,0,0,0.9)] backdrop-blur-xl">
          {notice !== null && (
            <div className="mb-5 rounded-[4px] border border-warn/30 bg-warn/[0.07] px-3 py-2.5 text-xs leading-relaxed text-fg-mid">
              {notice}
            </div>
          )}

          <div className="flex flex-col gap-2.5">
            <SsoButton
              label="Continue with Google"
              badge={lastUsed === 'google'}
              loading={pending === 'google'}
              disabled={busy}
              onClick={() => startSso('google')}
              icon={<GoogleIcon />}
            />
            <SsoButton
              label="Continue with GitHub"
              badge={lastUsed === 'github'}
              loading={pending === 'github'}
              disabled={busy}
              onClick={() => startSso('github')}
              icon={<GitHubIcon />}
            />
          </div>

          <div className="my-5 flex items-center gap-3">
            <span className="h-px flex-1 bg-line" />
            <span className="text-[10px] uppercase tracking-[0.2em] text-fg-dim">or</span>
            <span className="h-px flex-1 bg-line" />
          </div>

          <form onSubmit={submitEmail} className="flex flex-col gap-3">
            <div>
              <input
                type="email"
                value={email}
                placeholder="you@desk.com"
                autoComplete="email"
                onChange={(event) => {
                  setEmail(event.target.value)
                  if (invalid !== null) setInvalid(null)
                }}
                className={cn(
                  'h-11 w-full rounded-[4px] border bg-canvas/60 px-3.5 text-sm text-fg outline-none transition-colors',
                  'placeholder:text-fg-dim focus:border-accent/70',
                  invalid !== null ? 'border-down' : 'border-line',
                )}
              />
              {invalid !== null && <p className="mt-1.5 text-xs text-down">{invalid}</p>}
            </div>
            <button
              type="submit"
              disabled={busy}
              className={cn(
                'flex h-11 w-full items-center justify-center gap-2 rounded-[4px] text-sm font-medium',
                // Dark on cyan, as the mark itself is: light text on this
                // cyan is the one low-contrast pair in the palette.
                'bg-accent text-canvas transition-[transform,background-color] duration-150',
                'hover:bg-accent-deep active:scale-[0.985] disabled:opacity-60',
              )}
            >
              {pending === 'email' ? <Spinner /> : 'Send a sign-in link'}
            </button>
          </form>

          <p className="mt-5 text-center text-3xs text-fg-mid">
            Every order carries the identity that placed it.
          </p>
        </div>

        <button
          type="button"
          onClick={() => void navigate('/')}
          className="mt-6 text-sm text-fg-mid underline decoration-line underline-offset-4 transition-colors hover:text-fg hover:decoration-muted"
        >
          Continue without signing in
        </button>
        {/* The gate is not wired yet, and the link says so in one line rather
            than letting a reader assume it is. */}
        <p className="mt-2 text-center text-3xs text-fg-dim">This screen gates nothing yet.</p>
      </div>
    </div>
  )
}

/** There is no auth endpoint in the contract. Saying which is missing beats a
 *  spinner that resolves into a screen the user never signed in to. */
function unavailable(
  setPending: (value: null) => void,
  setNotice: (value: string) => void,
): void {
  window.setTimeout(() => {
    setPending(null)
    setNotice(
      'admind serves no auth endpoint yet — schema/admin-api.yaml describes a read-only v1 on loopback. Nothing was sent.',
    )
  }, 700)
}

function SsoButton({
  label,
  icon,
  badge,
  loading,
  disabled,
  onClick,
}: {
  label: string
  icon: React.ReactNode
  badge: boolean
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
        'relative flex h-11 w-full items-center justify-center gap-2.5 rounded-[4px] border border-line',
        'bg-raised/60 text-sm font-medium text-fg transition-[transform,background-color] duration-150',
        'hover:bg-raised active:scale-[0.985] disabled:opacity-60',
      )}
    >
      {loading ? <Spinner /> : icon}
      <span>{label}</span>
      <span
        className={cn(
          'pointer-events-none absolute -top-2 -right-2 rounded-[3px] bg-raised px-1.5 py-0.5',
          'text-[10px] leading-none text-fg-mid ring-1 ring-line transition-all duration-300',
          badge ? 'scale-100 opacity-100' : 'scale-95 opacity-0',
        )}
      >
        last used
      </span>
    </button>
  )
}

function Spinner() {
  return (
    <svg viewBox="0 0 24 24" className="size-4 animate-spin" aria-label="Working">
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

/** Two soft lights and a rule grid. Everything is a gradient — no image asset,
 *  so the screen paints on the first frame and there is nothing to 404. */
function Backdrop() {
  return (
    <div aria-hidden className="pointer-events-none absolute inset-0 overflow-hidden">
      <div className="absolute -top-40 left-1/2 size-[520px] -translate-x-1/2 rounded-full bg-brand/20 blur-[120px]" />
      <div className="absolute -bottom-56 right-[-10%] size-[420px] rounded-full bg-[#0284c7]/15 blur-[120px]" />
      <div
        className="absolute inset-0 opacity-[0.55]"
        style={{
          backgroundImage:
            'linear-gradient(#1d293a 1px, transparent 1px), linear-gradient(90deg, #1d293a 1px, transparent 1px)',
          backgroundSize: '48px 48px',
          maskImage: 'radial-gradient(ellipse 80% 60% at 50% 40%, #000 0%, transparent 75%)',
          WebkitMaskImage:
            'radial-gradient(ellipse 80% 60% at 50% 40%, #000 0%, transparent 75%)',
          opacity: 0.25,
        }}
      />
    </div>
  )
}

function GoogleIcon() {
  return (
    <svg viewBox="0 0 24 24" className="size-4" aria-hidden="true">
      <path
        fill="#4285F4"
        d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z"
      />
      <path
        fill="#34A853"
        d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"
      />
      <path
        fill="#FBBC05"
        d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"
      />
      <path
        fill="#EA4335"
        d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"
      />
    </svg>
  )
}

function GitHubIcon() {
  return (
    <svg viewBox="0 0 24 24" className="size-4" fill="currentColor" aria-hidden="true">
      <path d="M12 0C5.37 0 0 5.37 0 12c0 5.31 3.435 9.795 8.205 11.385.6.105.825-.255.825-.57 0-.285-.015-1.23-.015-2.235-3.015.555-3.795-.735-4.035-1.41-.135-.345-.72-1.41-1.23-1.695-.42-.225-1.02-.78-.015-.795.945-.015 1.62.87 1.845 1.23 1.08 1.815 2.805 1.305 3.495.99.105-.78.42-1.305.765-1.605-2.67-.3-5.46-1.335-5.46-5.925 0-1.305.465-2.385 1.23-3.225-.12-.3-.54-1.53.12-3.18 0 0 1.005-.315 3.3 1.23.96-.27 1.98-.405 3-.405s2.04.135 3 .405c2.295-1.56 3.3-1.23 3.3-1.23.66 1.65.24 2.88.12 3.18.765.84 1.23 1.905 1.23 3.225 0 4.605-2.805 5.625-5.475 5.925.435.375.81 1.095.81 2.22 0 1.605-.015 2.895-.015 3.3 0 .315.225.69.825.57A12.02 12.02 0 0024 12c0-6.63-5.37-12-12-12z" />
    </svg>
  )
}
