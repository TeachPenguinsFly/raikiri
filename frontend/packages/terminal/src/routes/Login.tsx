import { lazy, Suspense, type ReactNode } from 'react'
import { Link } from 'react-router'
import { BrandMark, cn } from '@raikiri/ui'

/** lazy so the shader stays out of the main bundle */
const Lightning = lazy(() => import('@raikiri/ui/lightning'))

/** hue of the brand cyan #38bdf8 */
const k_accent_hue = 199

export function Login({
  brand = <BrandMark className="text-lg" />,
}: {
  brand?: ReactNode | undefined
}) {
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
        {brand}

        <div className="panel mt-6 w-full p-4">
          <Link
            to="/"
            className={cn(
              'flex h-9 w-full items-center justify-center rounded-md bg-raised',
              'text-sm font-medium text-fg transition-[transform,background-color] duration-150',
              'hover:bg-hover active:scale-[0.99]',
            )}
          >
            Open the terminal
          </Link>
        </div>
      </div>
    </div>
  )
}

function Backdrop() {
  return (
    <div aria-hidden className="pointer-events-none absolute inset-0 overflow-hidden">
      <Suspense fallback={null}>
        <div className="absolute inset-0">
          <Lightning hue={k_accent_hue} speed={0.7} intensity={1.2} />
        </div>
      </Suspense>
    </div>
  )
}
