import { useEffect, useState, useSyncExternalStore } from 'react'
import { n } from '@raikiri/ui/v3'
import {
  api,
  k_base,
  type Book,
  type Funding,
  type Quote,
  type Schema,
  type StreamFrame,
  type Ticker,
  type Trade,
} from './client'

// one socket for the product: a subscription lives while any hook holds it, and every reopen sends the live set
// again. Each quote also lands on the tape the chart draws from, since the stream carries no candles

export type Greeks = Schema<'Greeks'>

type RowChannel = 'quotes' | 'funding' | 'ticker' | 'greeks'
type Request =
  | { channel: RowChannel; instruments: string[]; hz?: number | undefined }
  | { channel: 'book'; instrument: string; levels: number }
  | { channel: 'trades'; instrument: string }
type Entry = { id: string; request: Request; listeners: Set<(frame: StreamFrame) => void> }

const k_reconnect_ms = 1_000
const k_book_flush_ms = 100
/** the server refuses more per subscription, so a long list goes as several */
const k_chunk = 500
const k_tape_max = 40_000
const k_trades_max = 100

const k_tape = new Map<string, Array<[number, number]>>()

/** each move of an instrument's mid, [ms, mid] in time order */
export const tapeOf = (id: string): ReadonlyArray<readonly [number, number]> => k_tape.get(id) ?? []

function record(q: Quote) {
  const bid = n(q.bid)
  const ask = n(q.ask)
  if (bid === undefined || ask === undefined) return
  const t = Number(q.ts_event) / 1e6
  const mid = (bid + ask) / 2
  let tape = k_tape.get(q.instrument_id)
  if (tape === undefined) k_tape.set(q.instrument_id, (tape = []))
  const last = tape.at(-1)
  if (last !== undefined && (t < last[0] || last[1] === mid)) return
  tape.push([t, mid])
  if (tape.length > k_tape_max) tape.splice(0, tape.length - k_tape_max)
}

class Socket {
  private ws: WebSocket | null = null
  private connecting = false
  private retry: ReturnType<typeof setTimeout> | undefined
  private idle: ReturnType<typeof setTimeout> | undefined
  private nextId = 1
  private readonly subs = new Map<string, Entry>()
  private readonly watchers = new Set<() => void>()
  down = false

  subscribe(request: Request, onFrame: (frame: StreamFrame) => void): () => void {
    const key = JSON.stringify(request)
    let entry = this.subs.get(key)
    if (entry === undefined) {
      entry = { id: `s${this.nextId++}`, request, listeners: new Set() }
      this.subs.set(key, entry)
      this.send(message(entry))
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

  private setDown(down: boolean) {
    if (this.down === down) return
    this.down = down
    for (const w of this.watchers) w()
  }

  private open() {
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

  private reconnect() {
    if (this.subs.size > 0) this.retry = setTimeout(() => this.subs.size > 0 && this.open(), k_reconnect_ms)
  }

  private connect(token: string) {
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(`${scheme}://${location.host}${k_base}/v1/stream?token=${encodeURIComponent(token)}`)
    this.ws = ws
    ws.onopen = () => {
      if (this.ws !== ws) return
      this.setDown(false)
      for (const entry of this.subs.values()) ws.send(message(entry))
    }
    ws.onmessage = (event) => {
      if (this.ws !== ws) return
      const frame = JSON.parse(String(event.data)) as StreamFrame
      if (frame.type === 'quotes') for (const q of frame.quotes) record(q)
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

  private close() {
    clearTimeout(this.retry)
    const ws = this.ws
    this.ws = null
    ws?.close()
  }

  private send(m: string) {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(m)
  }
}

const message = ({ id, request }: Entry) =>
  JSON.stringify(
    request.channel === 'book'
      ? { op: 'subscribe', id, book: request.instrument, levels: request.levels }
      : request.channel === 'trades'
        ? { op: 'subscribe', id, trades: request.instrument }
        : { op: 'subscribe', id, [request.channel]: request.instruments, hz: request.hz },
  )

const socket = new Socket()

export function useStreamDown(): boolean {
  return useSyncExternalStore(
    (onChange) => socket.watch(onChange),
    () => socket.down,
  )
}

/** a frame carries only what moved, so rows merge by instrument, and the frames of one animation frame render once */
function useRows<T extends { instrument_id: string }>(
  channel: RowChannel,
  instruments: readonly string[],
  read: (frame: StreamFrame) => T[] | null,
  hz?: number,
): Map<string, T> {
  const [rows, setRows] = useState(() => new Map<string, T>())
  const signature = instruments.length === 0 ? null : JSON.stringify([...instruments].sort())
  useEffect(() => {
    if (signature === null) return
    const ids = JSON.parse(signature) as string[]
    let pending = new Map<string, T>()
    let scheduled = 0
    const flush = () => {
      scheduled = 0
      const batch = pending
      pending = new Map()
      setRows((prev) => {
        const next = new Map(prev)
        for (const [id, row] of batch) next.set(id, row)
        return next
      })
    }
    const stops: Array<() => void> = []
    for (let i = 0; i < ids.length; i += k_chunk) {
      stops.push(
        socket.subscribe({ channel, instruments: ids.slice(i, i + k_chunk), hz }, (frame) => {
          const got = read(frame)
          if (got === null) return
          for (const row of got) pending.set(row.instrument_id, row)
          if (scheduled === 0) scheduled = requestAnimationFrame(flush)
        }),
      )
    }
    return () => {
      cancelAnimationFrame(scheduled)
      for (const stop of stops) stop()
    }
  }, [channel, signature, hz])
  return rows
}

const readQuotes = (f: StreamFrame) => (f.type === 'quotes' ? f.quotes : null)
const readFunding = (f: StreamFrame) => (f.type === 'funding' ? f.funding : null)
const readTicker = (f: StreamFrame) => (f.type === 'ticker' ? f.ticker : null)
const readGreeks = (f: StreamFrame) => (f.type === 'greeks' ? f.greeks : null)

/** a chain is hundreds of contracts; a few frames a second of what moved, where the server's own pace is one */
const k_greeks_hz = 4

export const useQuotes = (ids: readonly string[]): Map<string, Quote> => useRows('quotes', ids, readQuotes)
export const useFunding = (ids: readonly string[]): Map<string, Funding> => useRows('funding', ids, readFunding)
export const useTicker = (ids: readonly string[]): Map<string, Ticker> => useRows('ticker', ids, readTicker)
/** each option's venue greeks, ivs and mark as the venue last sent them */
export const useGreeks = (ids: readonly string[]): Map<string, Greeks> => useRows('greeks', ids, readGreeks, k_greeks_hz)

/** each instrument's ladder, the latest frame of each */
export function useBooks(ids: readonly string[], levels: number): Map<string, Book> {
  const [books, setBooks] = useState(() => new Map<string, Book>())
  const signature = JSON.stringify(ids)
  useEffect(() => {
    const list = JSON.parse(signature) as string[]
    setBooks(new Map())
    // thirty venues' ladders each arrive several times a second; one render per frame of them, not per frame each
    const held = new Map<string, Book>()
    let dirty = false
    const stops = list.map((id) =>
      socket.subscribe({ channel: 'book', instrument: id, levels }, (f) => {
        if (f.type !== 'book') return
        held.set(f.book.instrument_id, f.book)
        dirty = true
      }),
    )
    const flush = setInterval(() => {
      if (!dirty) return
      dirty = false
      setBooks(new Map(held))
    }, k_book_flush_ms)
    return () => {
      clearInterval(flush)
      stops.forEach((s) => s())
    }
  }, [signature, levels])
  return books
}

/** a print with the order it arrived in, which a row is keyed by */
export type TapeTrade = Trade & { seq: number }

let k_trade_seq = 0

/** an instrument's last prints, newest first; a snapshot, sent again on every reopen, replaces what is held */
export function useTrades(id: string | undefined): TapeTrade[] {
  const [trades, setTrades] = useState<TapeTrade[]>([])
  useEffect(() => {
    setTrades([])
    if (id === undefined) return
    return socket.subscribe({ channel: 'trades', instrument: id }, (f) => {
      if (f.type !== 'trades') return
      const fresh = f.trades.map((t) => ({ ...t, seq: ++k_trade_seq })).reverse()
      setTrades((prev) => (f.snapshot ? fresh : [...fresh, ...prev]).slice(0, k_trades_max))
    })
  }, [id])
  return trades
}
