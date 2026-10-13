import { useEffect, useState, useSyncExternalStore } from 'react'
import { api } from '../api/client'
import type { components } from '../api/types'

type Frame = components['schemas']['StreamFrame']
type Quote = components['schemas']['Quote']
type Funding = components['schemas']['Funding']
type Ticker = components['schemas']['Ticker']
type Book = components['schemas']['Book']

type Request =
  | { channel: 'quotes' | 'funding' | 'ticker'; instruments: string[] }
  | { channel: 'book'; instrument: string; levels: number }

type Entry = { id: string; request: Request; listeners: Set<(frame: Frame) => void> }

const k_reconnect_ms = 1000

/** one shared socket; a subscription lives while any listener holds it */
class Socket {
  private ws: WebSocket | null = null
  /** true while the token for the next socket is being fetched */
  private connecting = false
  private retry: ReturnType<typeof setTimeout> | undefined
  private idle: ReturnType<typeof setTimeout> | undefined
  private nextId = 1
  private readonly subs = new Map<string, Entry>()
  private readonly watchers = new Set<() => void>()
  /** true from a close until the next open */
  down = false

  subscribe(request: Request, onFrame: (frame: Frame) => void): () => void {
    const key = JSON.stringify(request)
    let entry = this.subs.get(key)
    if (entry === undefined) {
      entry = { id: `s${this.nextId++}`, request, listeners: new Set() }
      this.subs.set(key, entry)
      this.send(subscribeMessage(entry))
    }
    entry.listeners.add(onFrame)
    clearTimeout(this.idle)
    this.open()

    return () => {
      const live = this.subs.get(key)
      if (live === undefined) return
      live.listeners.delete(onFrame)
      if (live.listeners.size > 0) return
      this.subs.delete(key)
      this.send(JSON.stringify({ op: 'unsubscribe', id: live.id }))
      // deferred so a page that resubscribes in the same commit keeps the socket
      if (this.subs.size === 0) this.idle = setTimeout(() => this.subs.size === 0 && this.close())
    }
  }

  watch(onChange: () => void): () => void {
    this.watchers.add(onChange)
    return () => this.watchers.delete(onChange)
  }

  private setDown(down: boolean): void {
    if (this.down === down) return
    this.down = down
    for (const watcher of this.watchers) watcher()
  }

  private open(): void {
    if (this.ws !== null || this.connecting) return
    clearTimeout(this.retry)
    this.connecting = true
    api.streamToken().then(
      ({ token }) => {
        this.connecting = false
        if (this.ws === null && this.subs.size > 0) this.connect(token)
      },
      () => {
        this.connecting = false
        this.setDown(true)
        this.reconnect()
      },
    )
  }

  private reconnect(): void {
    if (this.subs.size > 0) this.retry = setTimeout(() => this.subs.size > 0 && this.open(), k_reconnect_ms)
  }

  private connect(token: string): void {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(`${scheme}://${location.host}/v1/stream?token=${encodeURIComponent(token)}`)
    this.ws = ws
    // the server forgets a closed connection, so every open resends the live set
    ws.onopen = () => {
      if (this.ws !== ws) return
      this.setDown(false)
      for (const entry of this.subs.values()) ws.send(subscribeMessage(entry))
    }
    ws.onmessage = (event) => {
      if (this.ws !== ws) return
      const frame = JSON.parse(String(event.data)) as Frame
      for (const entry of this.subs.values()) {
        if (entry.id === frame.id) for (const listener of entry.listeners) listener(frame)
      }
    }
    ws.onclose = () => {
      if (this.ws !== ws) return
      this.ws = null
      this.setDown(true)
      this.reconnect()
    }
    ws.onerror = () => ws.close()
  }

  private close(): void {
    clearTimeout(this.retry)
    const ws = this.ws
    this.ws = null
    ws?.close()
  }

  /** before the socket opens there is nothing to send; onopen sends the live set */
  private send(message: string): void {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(message)
  }
}

function subscribeMessage({ id, request }: Entry): string {
  return JSON.stringify(
    request.channel === 'book'
      ? { op: 'subscribe', id, book: request.instrument, levels: request.levels }
      : { op: 'subscribe', id, [request.channel]: request.instruments },
  )
}

const socket = new Socket()

export function useStreamDown(): boolean {
  return useSyncExternalStore(
    (onChange) => socket.watch(onChange),
    () => socket.down,
  )
}

/** keyed by value so a rebuilt request does not resubscribe; a new request starts empty */
function useFeed<T>(request: Request | null, read: (frame: Frame) => T | null): T | null {
  const [value, setValue] = useState<T | null>(null)
  const signature = request === null ? null : JSON.stringify(request)

  useEffect(() => {
    setValue(null)
    if (signature === null) return
    return socket.subscribe(JSON.parse(signature) as Request, (frame) => {
      const next = read(frame)
      if (next !== null) setValue(next)
    })
  }, [signature])

  return value
}

/** a frame carries only what moved, so rows merge by instrument */
function useRows<T extends { instrument_id: string }>(
  channel: 'quotes' | 'funding' | 'ticker',
  instruments: string[],
  read: (frame: Frame) => T[] | null,
): Map<string, T> {
  const [rows, setRows] = useState(() => new Map<string, T>())
  const signature =
    instruments.length === 0 ? null : JSON.stringify({ channel, instruments: [...instruments].sort() })

  useEffect(() => {
    setRows((prev) => (prev.size === 0 ? prev : new Map()))
    if (signature === null) return
    return socket.subscribe(JSON.parse(signature) as Request, (frame) => {
      const next = read(frame)
      if (next === null) return
      setRows((prev) => {
        const merged = new Map(prev)
        for (const row of next) merged.set(row.instrument_id, row)
        return merged
      })
    })
  }, [signature])

  return rows
}

const readQuotes = (frame: Frame) => (frame.type === 'quotes' ? frame.quotes : null)
const readFunding = (frame: Frame) => (frame.type === 'funding' ? frame.funding : null)
const readTicker = (frame: Frame) => (frame.type === 'ticker' ? frame.ticker : null)
const readBook = (frame: Frame) => (frame.type === 'book' ? frame.book : null)

export function useQuotes(instruments: string[]): Map<string, Quote> {
  return useRows('quotes', instruments, readQuotes)
}

export function useFunding(instruments: string[]): Map<string, Funding> {
  return useRows('funding', instruments, readFunding)
}

export function useTicker(instruments: string[]): Map<string, Ticker> {
  return useRows('ticker', instruments, readTicker)
}

export function useBook(instrument: string | null, levels: number): Book | null {
  return useFeed(instrument === null ? null : { channel: 'book', instrument, levels }, readBook)
}
