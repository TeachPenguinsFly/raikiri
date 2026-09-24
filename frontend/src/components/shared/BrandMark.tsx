import { cn } from '@/lib/utils'

export function BrandMark({ className }: { className?: string }) {
  return (
    <span
      className={cn(
        'inline-flex items-center font-extrabold tracking-[0.03em] text-fg',
        className,
      )}
    >
      TPF
      <span className="mx-[5px] inline-block size-[9px] rounded-[1px] bg-accent" />
      LABS
    </span>
  )
}

export function BrandLockup({ className }: { className?: string }) {
  return (
    <div className={cn('flex flex-col items-center', className)}>
      <BrandMark className="text-xl" />
    </div>
  )
}
