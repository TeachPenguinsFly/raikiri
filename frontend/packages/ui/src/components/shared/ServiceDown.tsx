export function ServiceDown({ detail }: { detail?: string | undefined }) {
  return (
    <div className="flex h-full flex-col items-center justify-center gap-3 text-center">
      <div className="text-[96px] font-semibold leading-none tracking-[-0.04em] text-fg-dim">
        500
      </div>
      <div className="text-sm text-fg">admind is not answering</div>
      <div className="max-w-[38ch] text-2xs leading-relaxed text-fg-dim">
        Nothing on this screen is current. Bring the stack up with{' '}
        <span className="num text-fg-mid">make up</span> from the repository root.
      </div>
      {detail === undefined ? null : (
        <div className="num max-w-[52ch] break-words text-2xs text-fg-dim/70">{detail}</div>
      )}
    </div>
  )
}
