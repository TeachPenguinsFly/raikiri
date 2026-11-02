from __future__ import annotations

import dataclasses
import types
import typing
from dataclasses import dataclass, field
from decimal import Decimal
from enum import Enum
from typing import Any, Generic, Literal, TypeVar, Union


class Side(str, Enum):
    BUY = "buy"
    SELL = "sell"


class OrderType(str, Enum):
    LIMIT = "limit"
    MARKET = "market"


class TimeInForce(str, Enum):
    GTC = "gtc"
    IOC = "ioc"
    FOK = "fok"


class OrderStatus(str, Enum):
    INITIALIZED = "initialized"
    DENIED = "denied"
    SUBMITTED = "submitted"
    ACCEPTED = "accepted"
    PARTIALLY_FILLED = "partially_filled"
    FILLED = "filled"
    CANCELED = "canceled"
    REJECTED = "rejected"
    EXPIRED = "expired"
    TRIGGERED = "triggered"
    PENDING_UPDATE = "pending_update"
    PENDING_CANCEL = "pending_cancel"


TERMINAL_ORDER = {
    OrderStatus.DENIED,
    OrderStatus.FILLED,
    OrderStatus.CANCELED,
    OrderStatus.REJECTED,
    OrderStatus.EXPIRED,
}

TERMINAL_ALGO = {"done", "cancelled", "failed"}

DecimalLike = Union[str, int, Decimal]
InstrumentRef = Union[str, "Instrument"]


def to_decimal(value: DecimalLike, name: str = "value") -> Decimal:
    """Exact decimal from str, int or Decimal. A float is refused: 0.1 is not one tenth."""
    if isinstance(value, bool) or isinstance(value, float):
        raise TypeError(f"{name} must be str, int or Decimal, not {type(value).__name__}")
    decimal = value if isinstance(value, Decimal) else Decimal(value)
    if not decimal.is_finite():
        raise ValueError(f"{name} must be finite")
    return decimal


def decimal_text(value: DecimalLike, name: str = "value") -> str:
    return format(to_decimal(value, name), "f")


def instrument_id(instrument: InstrumentRef) -> str:
    return instrument.instrument_id if isinstance(instrument, Instrument) else str(instrument)


T = TypeVar("T")


@dataclass(frozen=True)
class Page(Generic[T]):
    items: list[T]
    next_cursor: str | None = None


@dataclass(frozen=True)
class Instrument:
    instrument_id: str
    account: str
    venue: str
    market: str
    exchange_symbol: str
    local_symbol: str
    tick_size: Decimal
    lot_size: Decimal
    min_notional: Decimal
    contract_value: Decimal
    contract_kind: str
    asset_class: str
    asset_key: str | None = None
    expiry: int | None = None
    strike: Decimal | None = None
    put_call: str | None = None


@dataclass(frozen=True)
class Listing:
    instrument_id: str
    account: str
    venue: str
    market: str
    exchange_symbol: str
    local_symbol: str


@dataclass(frozen=True)
class Market:
    key: str
    symbol: str
    listings: list[Listing] = field(default_factory=list)


@dataclass(frozen=True)
class Account:
    account: str
    venue: str
    market: str
    alive: bool = False


@dataclass(frozen=True)
class OrderAck:
    client_order_id: str
    status: str
    client_id: str = ""
    replaces: str | None = None

    @property
    def id(self) -> str:
        return self.client_order_id


@dataclass(frozen=True)
class CancelAck:
    client_order_id: str
    status: str

    @property
    def id(self) -> str:
        return self.client_order_id


@dataclass(frozen=True)
class CancelAll:
    canceled: list[str] = field(default_factory=list)
    failed: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class OrderRefusal:
    """Why a denied order was refused, in the shape of a refused call's problem. ``code`` is stable."""

    code: str
    title: str
    status: int
    type: str = ""


@dataclass(frozen=True)
class Order:
    client_order_id: str
    instrument_id: str
    side: Side
    order_type: OrderType
    tif: TimeInForce
    qty: Decimal
    status: OrderStatus
    ts_created: int
    ts_updated: int
    exchange_order_id: str | None = None
    post_only: bool = False
    reduce_only: bool = False
    price: Decimal | None = None
    leaves: Decimal | None = None
    reject_class: str | None = None
    reject_code: int | None = None
    client_id: str | None = None
    account: str | None = None
    symbol: str | None = None
    parent_id: str | None = None
    book: str | None = None
    external_ref: str | None = None
    refusal: OrderRefusal | None = None

    @property
    def id(self) -> str:
        return self.client_order_id

    @property
    def is_open(self) -> bool:
        return self.status not in TERMINAL_ORDER

    @property
    def filled(self) -> Decimal:
        return self.qty - (self.leaves if self.leaves is not None else self.qty)


@dataclass(frozen=True)
class OrderEvent:
    client_order_id: str
    instrument_id: str
    event_type: str
    ts_event: int
    price: Decimal | None = None
    qty: Decimal | None = None
    leaves: Decimal | None = None
    fee: Decimal | None = None
    exec_id: str | None = None
    client_id: str | None = None
    account: str | None = None
    symbol: str | None = None
    parent_id: str | None = None
    seq: int | None = None


@dataclass(frozen=True)
class Fill:
    venue: str
    exec_id: str
    instrument_id: str
    side: Side
    price: Decimal
    qty: Decimal
    ts_event: int
    client_order_id: str | None = None
    exchange_order_id: str | None = None
    fee: Decimal | None = None
    liquidity: str | None = None
    client_id: str | None = None
    account: str | None = None
    symbol: str | None = None
    parent_id: str | None = None
    seq: int | None = None


@dataclass(frozen=True)
class Position:
    client_id: str
    account: str
    symbol: str
    instrument_id: str
    qty: Decimal
    realized: Decimal
    fees: Decimal
    ts_updated: int
    avg_entry_price: Decimal | None = None
    seq: int | None = None


@dataclass(frozen=True)
class Balance:
    asset: str
    total: Decimal | None = None
    available: Decimal | None = None
    unrealized: Decimal | None = None


@dataclass(frozen=True)
class AccountBalances:
    account: str
    venue: str
    stale: bool = False
    reported_at: int | None = None
    wallet: str | None = None
    assets: list[Balance] = field(default_factory=list)
    seq: int | None = None


@dataclass(frozen=True)
class ChildOrder:
    client_order_id: str
    instrument_id: str
    venue: str
    qty: Decimal
    limit: Decimal
    filled: Decimal
    fees: Decimal
    sent: int
    end: str
    open: bool
    avg_price: Decimal | None = None
    acked: int | None = None
    closed: int | None = None


@dataclass(frozen=True)
class AlgoOrder:
    id: str
    symbol: str
    market: str
    side: Side
    algo: str
    profile: str
    status: str
    qty: Decimal
    filled: Decimal
    working: Decimal
    fees: Decimal
    horizon_s: int
    slices: int
    slices_fired: int
    pov_bp: int
    created: int
    child_count: int
    reason: str | None = None
    limit: Decimal | None = None
    arrival: Decimal | None = None
    avg_price: Decimal | None = None
    market_vwap: Decimal | None = None
    slippage_arrival_bp: Decimal | None = None
    slippage_vwap_bp: Decimal | None = None
    started: int | None = None
    ended: int | None = None
    children: list[ChildOrder] | None = None
    client_id: str | None = None
    book: str | None = None
    external_ref: str | None = None
    seq: int | None = None

    @property
    def done(self) -> bool:
        return self.status in TERMINAL_ALGO

    @property
    def progress(self) -> Decimal:
        """Filled share of the target, 0 to 1. Zero until the target is known."""
        return self.filled / self.qty if self.qty > 0 else Decimal(0)


@dataclass(frozen=True)
class BookLevel:
    price: Decimal
    size: Decimal


@dataclass(frozen=True)
class Quote:
    instrument_id: str
    bid: Decimal
    bid_size: Decimal
    ask: Decimal
    ask_size: Decimal
    ts_event: int

    @property
    def mid(self) -> Decimal:
        return (self.bid + self.ask) / 2


@dataclass(frozen=True)
class Book:
    instrument_id: str
    bids: list[BookLevel]
    asks: list[BookLevel]
    ts_event: int


@dataclass(frozen=True)
class Greeks:
    instrument_id: str
    delta: Decimal
    gamma: Decimal
    vega: Decimal
    theta: Decimal
    rho: Decimal
    mark_iv: Decimal
    bid_iv: Decimal
    ask_iv: Decimal
    underlying_price: Decimal
    mark_price: Decimal
    index_price: Decimal
    ts_event: int


@dataclass(frozen=True)
class Funding:
    instrument_id: str
    rate: Decimal
    predicted_rate: Decimal
    mark_price: Decimal
    next_funding_time: int
    ts_event: int
    index_price: Decimal | None = None
    interval_seconds: int | None = None
    accrual: str | None = None


@dataclass(frozen=True)
class Ticker:
    instrument_id: str
    volume_24h_base: Decimal
    volume_24h_quote: Decimal
    ts_event: int
    open_interest: Decimal | None = None


@dataclass(frozen=True)
class AlgoLostEvent:
    """The execution service forgot this algo. Its children are still ordinary orders."""

    id: str
    reason: str
    seq: int | None = None


@dataclass(frozen=True)
class Gap:
    """Events between ``from_seq`` and ``to_seq`` are gone. Refetch orders, fills and positions over REST."""

    reason: str
    from_seq: int | None = None
    to_seq: int | None = None


@dataclass(frozen=True)
class OptionQuote:
    venue: str
    instrument_id: str
    symbol: str
    premium_unit: str
    iv_source: str
    greeks_source: str
    ts_event: int
    bid: Decimal | None = None
    ask: Decimal | None = None
    bid_size: Decimal | None = None
    ask_size: Decimal | None = None
    mark: Decimal | None = None
    bid_usd: Decimal | None = None
    ask_usd: Decimal | None = None
    mark_usd: Decimal | None = None
    bid_iv: Decimal | None = None
    ask_iv: Decimal | None = None
    mark_iv: Decimal | None = None
    delta: Decimal | None = None
    gamma: Decimal | None = None
    vega: Decimal | None = None
    theta: Decimal | None = None
    greeks_unit: str | None = None
    open_interest: Decimal | None = None
    volume_24h: Decimal | None = None


@dataclass(frozen=True)
class OptionBest:
    venue: str
    usd: Decimal
    iv: Decimal | None = None
    size: Decimal | None = None


@dataclass(frozen=True)
class OptionSide:
    venues: list[OptionQuote] = field(default_factory=list)
    best_bid: OptionBest | None = None
    best_ask: OptionBest | None = None


@dataclass(frozen=True)
class OptionRow:
    strike: Decimal
    call: OptionSide
    put: OptionSide


@dataclass(frozen=True)
class OptionChain:
    underlying: str
    expiry: int
    date: str
    as_of: int
    seconds_to_expiry: int
    forward_source: str
    rows: list[OptionRow] = field(default_factory=list)
    years: Decimal | None = None
    forward: Decimal | None = None
    atm_strike: Decimal | None = None


@dataclass(frozen=True)
class OptionExpiry:
    expiry: int
    date: str
    strikes: int = 0
    venues: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class OptionVenue:
    venue: str
    options: int = 0


@dataclass(frozen=True)
class OptionUnderlying:
    underlying: str
    venues: list[OptionVenue] = field(default_factory=list)
    expiries: list[OptionExpiry] = field(default_factory=list)


@dataclass(frozen=True)
class Rate:
    requests_per_second: int
    burst: int
    mutations_per_second: int
    mutation_burst: int


@dataclass(frozen=True)
class Whoami:
    key_id: str
    client_id: str
    permissions: list[str]
    accounts: list[str]
    symbols: list[str]
    rate: Rate
    max_streams: int


StreamEvent = Union[
    Quote, Book, Greeks, Funding, Ticker, OrderEvent, Fill, Position, AccountBalances, AlgoOrder, AlgoLostEvent, Gap
]

_Kind = TypeVar("_Kind")
_hints: dict[type, dict[str, Any]] = {}


def _hints_of(cls: type) -> dict[str, Any]:
    found = _hints.get(cls)
    if found is None:
        found = typing.get_type_hints(cls)
        _hints[cls] = found
    return found


def _convert(kind: Any, value: Any) -> Any:
    if value is None:
        return None
    origin = typing.get_origin(kind)
    if origin in (Union, types.UnionType):
        for arm in typing.get_args(kind):
            if arm is not type(None):
                return _convert(arm, value)
        return None
    if origin is list:
        item = typing.get_args(kind)[0]
        return [_convert(item, entry) for entry in value]
    if origin is Literal:
        return value
    if kind is Decimal:
        return Decimal(str(value))
    if kind is int:
        return int(value)
    if kind is str:
        return str(value)
    if kind is bool:
        return bool(value)
    if isinstance(kind, type) and issubclass(kind, Enum):
        try:
            return kind(value)
        except ValueError:
            return value  # a status newer than this package stays readable
    if dataclasses.is_dataclass(kind):
        return build(typing.cast(type, kind), value)
    return value


def build(cls: type[_Kind], data: dict[str, Any]) -> _Kind:
    """Fields the package does not know are ignored, so the server can add them."""
    hints = _hints_of(cls)
    values: dict[str, Any] = {}
    for item in dataclasses.fields(typing.cast(Any, cls)):
        if item.name in data:
            values[item.name] = _convert(hints[item.name], data[item.name])
    return cls(**values)
