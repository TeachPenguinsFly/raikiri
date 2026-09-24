import { useEffect, useRef, useState } from 'react'
import type { components } from '@/api/types'

type Frame = components['schemas']['StreamFrame']
type Quote = components['schemas']['Quote']
type Funding = components['schemas']['Funding']
type Ticker = components['schemas']['Ticker']
type Book = components['schemas']['Book']

export type Request =
  | { channel: 'quotes'; instruments: string[]; hz?: number }
  | { channel: 'funding'; instruments: string[]; hz?: number }
  | { channel: 'ticker'; instruments: string[]; hz?: number }
  | { channel: 'book'; instrument: string; levels?: number; hz?: number }

/** one shared socket; subscriptions are reference-counted by request */
class Socket {
  private ws: WebSocket | null = null
  private opening = false
  private nextId = 1
  private readonly pending: string[] = []
  private readonly subs = new Map<
    string,
    { id: string; request: Request; refs: number; listeners: Set<(frame: Frame) => void> }
  >()

  private key(request: Request): string {
    return request.channel === 'book'
      ? `book:${request.instrument}:${request.levels ?? ''}`
      : `${request.channel}:${[...request.instruments].sort().join(',')}`
  }

  subscribe(request: Request, onFrame: (frame: Frame) => void): () => void {
    const key = this.key(request)
    let entry = this.subs.get(key)
    if (entry === undefined) {
      entry = { id: `s${this.nextId++}`, request, refs: 0, listeners: new Set() }
      this.subs.set(key, entry)
      this.send(this.subscribeMessage(entry.id, request))
    }
    entry.refs += 1
    entry.listeners.add(onFrame)
    this.open()

    return () => {
      const live = this.subs.get(key)
      if (live === undefined) return
      live.listeners.delete(onFrame)
      live.refs -= 1
      if (live.refs > 0) return
      this.subs.delete(key)
      this.send(JSON.stringify({ op: 'unsubscribe', id: live.id }))
      if (this.subs.size === 0) this.close()
    }
  }

  private subscribeMessage(id: string, request: Request): string {
    const body: Record<string, unknown> = { op: 'subscribe', id }
    if (request.channel === 'book') {
      body.book = request.instrument
      if (request.levels !== undefined) body.levels = request.levels
    } else {
      body[request.channel] = request.instruments
    }
    if (request.hz !== undefined) body.hz = request.hz
    return JSON.stringify(body)
  }

  private open(): void {
    if (this.ws !== null || this.opening) return
    this.opening = true
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/v1/stream`
    const ws = new WebSocket(url)
    this.ws = ws
    ws.onopen = () => {
      this.opening = false
      // the server forgets a closed connection, so resend every subscription on open
      for (const entry of this.subs.values()) {
        ws.send(this.subscribeMessage(entry.id, entry.request))
      }
      for (const message of this.pending.splice(0)) ws.send(message)
    }
    ws.onmessage = (event) => {
      const frame = JSON.parse(String(event.data)) as Frame & { id?: string }
      for (const entry of this.subs.values()) {
        if (entry.id === frame.id) {
          for (const listener of entry.listeners) listener(frame)
        }
      }
    }
    ws.onclose = () => {
      this.ws = null
      this.opening = false
      if (this.subs.size > 0) setTimeout(() => this.open(), 1000)
    }
    ws.onerror = () => ws.close()
  }

  private close(): void {
    this.ws?.close()
    this.ws = null
  }

  private send(message: string): void {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(message)
    else this.pending.push(message)
  }
}

const socket = new Socket()

/** request is compared by value so a rebuilt object does not resubscribe */
function useFeed<T>(request: Request | null, read: (frame: Frame) => T | null): T | null {
  const [value, setValue] = useState<T | null>(null)
  const readRef = useRef(read)
  readRef.current = read
  const signature = request === null ? null : JSON.stringify(request)

  useEffect(() => {
    if (signature === null) {
      setValue(null)
      return
    }
    const parsed = JSON.parse(signature) as Request
    return socket.subscribe(parsed, (frame) => {
      const next = readRef.current(frame)
      if (next !== null) setValue(next)
    })
  }, [signature])

  return value
}

/** merges frames in the listener into a new map; a frame carries only what moved */
function useRows<T extends { instrument_id: string }>(
  request: Request | null,
  read: (frame: Frame) => T[] | null,
): Map<string, T> {
  const [rows, setRows] = useState(() => new Map<string, T>())
  const readRef = useRef(read)
  readRef.current = read
  const signature = request === null ? null : JSON.stringify(request)

  useEffect(() => {
    if (signature === null) return
    return socket.subscribe(JSON.parse(signature) as Request, (frame) => {
      const next = readRef.current(frame)
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

export function useQuotes(instruments: string[], hz?: number): Map<string, Quote> {
  return useRows(
    instruments.length === 0
      ? null
      : { channel: 'quotes', instruments, ...(hz === undefined ? {} : { hz }) },
    (frame) => (frame.type === 'quotes' ? frame.quotes : null),
  )
}

export function useFunding(instruments: string[]): Map<string, Funding> {
  return useRows(
    instruments.length === 0 ? null : { channel: 'funding', instruments },
    (frame) => (frame.type === 'funding' ? frame.funding : null),
  )
}

export function useTicker(instruments: string[]): Map<string, Ticker> {
  return useRows(
    instruments.length === 0 ? null : { channel: 'ticker', instruments },
    (frame) => (frame.type === 'ticker' ? frame.ticker : null),
  )
}

export function useBook(instrument: string | null, levels = 25): Book | null {
  return useFeed(
    instrument === null ? null : { channel: 'book', instrument, levels },
    (frame) => (frame.type === 'book' ? frame.book : null),
  )
}
