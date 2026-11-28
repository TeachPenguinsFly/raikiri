// the crypto terminal's parts, for a desk that adds widgets of its own beside the public ones
export {
  api,
  ApiError,
  del,
  get,
  post,
  put,
  setApiBase,
  type Account,
  type Book,
  type Funding,
  type Health,
  type Instrument,
  type Market,
  type Order,
  type Position,
  type Quote,
  type Schema,
  type Ticker,
} from './api/client'
export { tapeOf, useGreeks, useQuotes, type Greeks } from './api/stream'
export * from './lib/liquidation'
export * from './lib/market'
export { BookWidget, useBook } from './terminal/book'
export { ChartWidget } from './terminal/chart'
export { MarketBar } from './terminal/header'
export { VenuePicker, VenuesWidget } from './terminal/panels'
export { TerminalProvider, useTerminal, type BookVenue, type Draft, type Terminal } from './terminal/state'
export { Tabs } from './terminal/tabs'
export { TerminalDesk, TerminalPage, type AccountTab, type DeskSlots } from './terminal/TerminalPage'
