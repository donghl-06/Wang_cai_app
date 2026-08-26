"""Small, deterministic AData-shaped fixtures for market ID contract tests."""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from typing import Any

import pandas as pd
import pytest

from wangcai_syn.wangcai_cpp import MultiBacktestEngine, Strategy, SymbolData


DATE = "2025-01-02"

ORDER_COLUMNS = [
    "date",
    "time",
    "sym",
    "price",
    "size",
    "side",
    "ordertype",
    "orderid",
    "channelno",
    "seqno",
    "bizindex",
    "updatetime",
]

TRADE_COLUMNS = [
    "date",
    "time",
    "sym",
    "price",
    "size",
    "bidorderid",
    "askorderid",
    "tradeid",
    "exectype",
    "tradebsflag",
    "channelno",
    "bizindex",
    "updatetime",
]


class MarketEventRecorder(Strategy):
    """Record only the public market callbacks and copy their scalar fields."""

    def __init__(self) -> None:
        super().__init__()
        self.timeline: list[dict[str, Any]] = []

    def getStrategyId(self) -> str:
        return "market_event_id_contract"

    def onTickEvent(self, _tick):
        return []

    def onOrderEvent(self, order):
        self.timeline.append(
            {
                "kind": "order",
                "Instrument": order.Instrument,
                "Time": order.Time,
                "ChannelNo": order.ChannelNo,
                "OrderNo": order.OrderNo,
                "Price": order.Price,
                "Volume": order.Volume,
                "Side": order.Side,
                "SeqNo": order.SeqNo,
                "BizIndex": order.BizIndex,
            }
        )
        return []

    def onTradeEvent(self, trade):
        self.timeline.append(
            {
                "kind": "trade",
                "Instrument": trade.Instrument,
                "Time": trade.Time,
                "ChannelNo": trade.ChannelNo,
                "TradeIndex": trade.TradeIndex,
                "Price": trade.Price,
                "Volume": trade.Volume,
                "ExecType": trade.ExecType,
                "BuyNo": trade.BuyNo,
                "SellNo": trade.SellNo,
                "TradeBSFlag": trade.TradeBSFlag,
                "BizIndex": trade.BizIndex,
            }
        )
        return []

    def onOrderCallback(self, _callback) -> None:
        pass

    def onTradeCallback(self, _callback) -> None:
        pass

    def onOrderFilled(self, _order_id, _price, _volume) -> None:
        pass

    def onOrderCancelled(self, _order_id, _reason) -> None:
        pass


def order_row(
    symbol: str,
    time: str,
    *,
    price: float,
    size: int,
    side: int,
    order_id: int,
    channel: int = 2012,
    sequence: int,
) -> dict[str, Any]:
    return {
        "date": DATE,
        "time": f"0 days {time}",
        "sym": symbol,
        "price": price,
        "size": size,
        "side": side,
        "ordertype": 2 if symbol.endswith(".SZ") else 0,
        "orderid": order_id,
        "channelno": channel,
        "seqno": sequence,
        "bizindex": sequence,
        "updatetime": f"{DATE} {time}",
    }


def trade_row(
    symbol: str,
    time: str,
    *,
    bid_order_id: int,
    ask_order_id: int,
    trade_id: int,
    trade_bs_flag: Any,
    sequence: int,
    channel: int = 2012,
    exec_type: Any = "2",
) -> dict[str, Any]:
    return {
        "date": DATE,
        "time": f"0 days {time}",
        "sym": symbol,
        "price": 0,
        "size": 0,
        "bidorderid": bid_order_id,
        "askorderid": ask_order_id,
        "tradeid": trade_id,
        "exectype": exec_type,
        "tradebsflag": trade_bs_flag,
        "channelno": channel,
        "bizindex": sequence,
        "updatetime": f"{DATE} {time}",
    }


def _tick_csv(symbol: str) -> str:
    # Keep the physical column order expected by InfoLoader: all ten bids first,
    # then all ten asks.  A single 09:25 tick moves the engine into continuous
    # auction before the synthetic orders below arrive.
    row: dict[str, Any] = {
        "date": DATE,
        "time": "0 days 09:25:00.000000",
        "sym": symbol,
        "prevclose": 10.0,
        "open": 10.0,
        "high": 10.0,
        "low": 10.0,
        "close": 10.0,
        "volume": 0,
        "turnover": 0,
        "tradecount": 0,
    }
    for level in range(1, 11):
        row[f"bid{level}"] = 0
        row[f"bsize{level}"] = 0
    for level in range(1, 11):
        row[f"ask{level}"] = 0
        row[f"asize{level}"] = 0
    row.update(avgbid=0, avgask=0, totalbsize=0, totalasize=0, iopv=0)
    return pd.DataFrame([row]).to_csv(index=False)


def _bar_csv(symbol: str) -> str:
    return pd.DataFrame(
        [
            {
                "sym": symbol,
                "prevclose": 10.0,
                "open": 10.0,
                "high": 10.0,
                "low": 10.0,
                "close": 10.0,
                "volume": 0,
                "turnover": 0,
                "tradecount": 0,
                "af": 1.0,
                "upperlimit": 11.0,
                "lowerlimit": 9.0,
            }
        ]
    ).to_csv(index=False)


def make_symbol_data(
    symbol: str,
    orders: Sequence[dict[str, Any]],
    trades: Iterable[dict[str, Any]] = (),
) -> SymbolData:
    order_csv = pd.DataFrame(orders, columns=ORDER_COLUMNS).to_csv(index=False)
    trade_csv = pd.DataFrame(list(trades), columns=TRADE_COLUMNS).to_csv(index=False)
    return SymbolData(
        symbol,
        _tick_csv(symbol),
        order_csv,
        trade_csv,
        _bar_csv(symbol),
        False,
    )


def run_multi_market_events(
    datasets: Sequence[
        tuple[
            str,
            Sequence[dict[str, Any]],
            Iterable[dict[str, Any]],
        ]
    ],
    recorder: MarketEventRecorder | None = None,
) -> list[dict[str, Any]]:
    symbol_data = [
        make_symbol_data(symbol, orders, trades)
        for symbol, orders, trades in datasets
    ]
    engine = MultiBacktestEngine(symbol_data)
    recorder = recorder or MarketEventRecorder()
    engine.registerStrategy(recorder)
    engine.run()
    return recorder.timeline


def run_market_events(
    symbol: str,
    orders: Sequence[dict[str, Any]],
    trades: Iterable[dict[str, Any]] = (),
    recorder: MarketEventRecorder | None = None,
) -> list[dict[str, Any]]:
    return run_multi_market_events(
        [(symbol, orders, trades)],
        recorder=recorder,
    )


@pytest.fixture
def run_events():
    return run_market_events


@pytest.fixture
def run_multi_events():
    return run_multi_market_events
