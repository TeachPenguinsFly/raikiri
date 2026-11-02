from __future__ import annotations

import json
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from decimal import Decimal
from typing import Any

from . import models as m
from .models import InstrumentRef, StreamEvent, build, instrument_id

PRIVATE_ID = "private"


@dataclass
class Subscriptions:
    """What a stream is subscribed to, in a form that survives a reconnect."""

    channels: set[str] = field(default_factory=set)
    market: dict[str, dict[str, Any]] = field(default_factory=dict)
    next_id: int = 0

    def market_message(
        self,
        *,
        quotes: Sequence[InstrumentRef] | InstrumentRef | None = None,
        book: InstrumentRef | None = None,
        levels: int | None = None,
        greeks: Sequence[InstrumentRef] | InstrumentRef | None = None,
        funding: Sequence[InstrumentRef] | InstrumentRef | None = None,
        ticker: Sequence[InstrumentRef] | InstrumentRef | None = None,
        hz: int | Decimal | None = None,
    ) -> dict[str, Any] | None:
        message: dict[str, Any] = {}
        for name, value in (("quotes", quotes), ("greeks", greeks), ("funding", funding), ("ticker", ticker)):
            if value is not None:
                message[name] = _ids(value)
        if book is not None:
            message["book"] = instrument_id(book)
            if levels is not None:
                message["levels"] = levels
        if not message:
            return None
        if hz is not None:
            message["hz"] = int(hz)
        self.next_id += 1
        market_id = f"m{self.next_id}"
        message.update(op="subscribe", id=market_id)
        self.market[market_id] = message
        return message


def _ids(value: Sequence[InstrumentRef] | InstrumentRef) -> list[str]:
    if isinstance(value, (str, m.Instrument)):
        return [instrument_id(value)]
    return [instrument_id(item) for item in value]


class StreamState:
    """The stream protocol without any socket: frames in, typed events and messages to send out."""

    def __init__(self, subscriptions: Subscriptions) -> None:
        self.subscriptions = subscriptions
        self.epoch: str | None = None
        self.last_seq: int | None = None
        self.heartbeat_s = 5.0
        self.server_epoch: str | None = None

    def private_message(self) -> dict[str, Any] | None:
        if not self.subscriptions.channels:
            return None
        message: dict[str, Any] = {"op": "subscribe", "id": PRIVATE_ID, "channels": sorted(self.subscriptions.channels)}
        if self.last_seq is not None and self.epoch is not None:
            message["resume_from"] = str(self.last_seq)
            message["epoch"] = self.epoch
        return message

    def on_open(self) -> list[dict[str, Any]]:
        """Everything to send on a new connection, after its hello."""
        out = list(self.subscriptions.market.values())
        private = self.private_message()
        if private is not None:
            out.insert(0, private)
        return out

    def on_frame(self, text: str) -> tuple[list[StreamEvent], list[dict[str, Any]], dict[str, Any] | None]:
        """Returns the events in this frame, messages to send now, and a problem if it carried one."""
        frame = json.loads(text)
        kind = frame.get("type")
        if kind == "hello":
            self.heartbeat_s = float(frame.get("heartbeat_ms", 5000)) / 1000
            # the subscribe below carries the epoch we resume from; a different one earns a gap from the server
            sends = self.on_open()
            self.server_epoch = str(frame.get("epoch", ""))
            if self.epoch is None:
                self.epoch = self.server_epoch
            return [], sends, None
        if kind in ("heartbeat", "ack", "pong"):
            return [], [], None
        if kind == "problem":
            return [], [], frame.get("problem", {})
        if kind == "gap":
            if "from" not in frame:
                self._note_seq(frame)  # the feed lost events before they were sequenced for us
                return [m.Gap(reason=str(frame.get("reason", "")))], [], None
            # our resume point is void: everything from here on counts in the server's current epoch
            self.epoch = self.server_epoch
            self.last_seq = None
            return [m.Gap(str(frame.get("reason", "")), int(frame["from"]), _opt_int(frame.get("to")))], [], None
        if kind == "quotes":
            return [build(m.Quote, row) for row in frame.get("quotes", [])], [], None
        if kind == "book":
            return [build(m.Book, frame["book"])], [], None
        if kind == "greeks":
            return [build(m.Greeks, row) for row in frame.get("greeks", [])], [], None
        if kind == "funding":
            return [build(m.Funding, row) for row in frame.get("funding", [])], [], None
        if kind == "ticker":
            return [build(m.Ticker, row) for row in frame.get("ticker", [])], [], None
        if kind in _PRIVATE:
            if self._duplicate(frame):
                return [], [], None
            model, field_name = _PRIVATE[kind]
            row = dict(frame[field_name])
            row["seq"] = frame.get("seq")
            return [build(model, row)], [], None
        return [], [], None  # a frame type newer than this package

    def _note_seq(self, frame: dict[str, Any]) -> None:
        seq = _opt_int(frame.get("seq"))
        if seq is not None:
            self.last_seq = seq if self.last_seq is None else max(self.last_seq, seq)

    def _duplicate(self, frame: dict[str, Any]) -> bool:
        seq = _opt_int(frame.get("seq"))
        if seq is None:
            return False
        if self.last_seq is not None and seq <= self.last_seq:
            return True
        self.last_seq = seq
        return False


def _opt_int(value: Any) -> int | None:
    return None if value is None else int(value)


_PRIVATE: dict[str, tuple[Any, str]] = {
    "order": (m.OrderEvent, "order_event"),
    "fill": (m.Fill, "fill"),
    "position": (m.Position, "position"),
    "balance": (m.AccountBalances, "balance"),
    "algo": (m.AlgoOrder, "algo"),
    "algo_lost": (m.AlgoLostEvent, "algo_lost"),
}


def channels_of(
    orders: bool, fills: bool, positions: bool, balances: bool, algos: bool, extra: Iterable[str] = ()
) -> set[str]:
    named = {
        "orders": orders,
        "fills": fills,
        "positions": positions,
        "balances": balances,
        "algos": algos,
    }
    return {name for name, wanted in named.items() if wanted} | set(extra)
