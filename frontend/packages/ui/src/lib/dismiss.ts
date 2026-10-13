import { useEffect, type RefObject } from 'react'

/** closes a popover on a press outside it or on escape */
export function useDismiss(
  box: RefObject<HTMLElement | null>,
  open: boolean,
  setOpen: (open: boolean) => void,
) {
  useEffect(() => {
    if (!open) return
    const away = (e: PointerEvent) => {
      if (box.current?.contains(e.target as Node) !== true) setOpen(false)
    }
    const escape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    document.addEventListener('pointerdown', away)
    document.addEventListener('keydown', escape)
    return () => {
      document.removeEventListener('pointerdown', away)
      document.removeEventListener('keydown', escape)
    }
  }, [box, open, setOpen])
}
