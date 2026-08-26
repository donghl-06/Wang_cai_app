"""
队列信息回调功能测试

验证点：
1) queue_info_enabled=False 时，新字段保持默认值，不影响原有字段
2) queue_info_enabled=True 时，新字段可用且结构正确
"""

from __future__ import annotations

from pathlib import Path
from typing import List

import pandas as pd

from wangcai_syn import (  # noqa: E402
    OrderCallback,
    OrderDetail,
    Snapshot,
    Strategy,
    TradeCallback,
    TradeDetail,
    UserEvent,
    make_order,
    run_backtest,
)


class QueueInfoProbeStrategy(Strategy):
    def __init__(self, account: str = "queue_probe"):
        super().__init__()
        self.account = account
        self.sent = False
        self.order_callbacks: List[OrderCallback] = []

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        if self.sent:
            return []
        if tick.Time < 93000000:
            return []

        bid1 = tick.bids[0] if tick.bids and tick.bids[0] > 0 else 0
        if bid1 <= 0:
            return []

        self.sent = True
        return [
            make_order(
                Broker="",
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=f"{self.account}_001",
                OrderType=0,
                Direction=1,  # 买入
                Price=bid1,   # 尽量走被动路径
                Volume=100,
            )
        ]

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTradeCallback(self, callback: TradeCallback) -> None:
        return None

    def onOrderCallback(self, callback: OrderCallback) -> None:
        self.order_callbacks.append(callback)


def load_synth_data():
    base = Path(__file__).resolve().parents[1] / "real_trade_mode" / "synth_data"
    symbol = "300827.SZ"
    date = "2025-01-02"
    cstick = pd.read_csv(base / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(base / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(base / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(base / f"csbar1d_{symbol}_{date}.csv")
    return symbol, {symbol: (cstick, csord, cstra, csbar1d)}


def assert_legacy_fields_unchanged(cb: OrderCallback) -> None:
    assert isinstance(cb.price, int), "原字段 price 类型异常"
    assert isinstance(cb.volume, int), "原字段 volume 类型异常"
    assert isinstance(cb.orderlocalid, str) and cb.orderlocalid, "原字段 orderlocalid 异常"


def run_case(queue_info_enabled: bool):
    symbol, data = load_synth_data()
    strategy = QueueInfoProbeStrategy(account=f"qinfo_{'on' if queue_info_enabled else 'off'}")
    ok = run_backtest(
        data_dict=data,
        strategy=strategy,
        queue_info_enabled=queue_info_enabled,
        output_dir=None,
    )
    assert ok, f"run_backtest 执行失败 (queue_info_enabled={queue_info_enabled})"
    assert strategy.order_callbacks, f"未收到下单回调 (queue_info_enabled={queue_info_enabled}, symbol={symbol})"
    cb = strategy.order_callbacks[0]
    assert_legacy_fields_unchanged(cb)
    return cb


def main() -> int:
    cb_off = run_case(queue_info_enabled=False)
    assert cb_off.queue_ahead_count == -1, "关闭开关时 queue_ahead_count 应为 -1"
    assert cb_off.queue_ahead_volume == -1, "关闭开关时 queue_ahead_volume 应为 -1"
    assert cb_off.prev_order_ids == [], "关闭开关时 prev_order_ids 应为空"

    cb_on = run_case(queue_info_enabled=True)
    assert cb_on.queue_ahead_count >= 0, "开启开关时 queue_ahead_count 应 >= 0"
    assert cb_on.queue_ahead_volume >= 0, "开启开关时 queue_ahead_volume 应 >= 0"
    assert isinstance(cb_on.prev_order_ids, list), "开启开关时 prev_order_ids 应为 list"
    assert len(cb_on.prev_order_ids) <= 3, "prev_order_ids 最多返回 3 个"
    for oid in cb_on.prev_order_ids:
        assert isinstance(oid, int) and oid > 0, f"历史订单ID非法: {oid}"

    print("✅ 队列信息回调测试通过")
    print(
        "   开启时: ahead_count={}, ahead_volume={}, prev_order_ids={}".format(
            cb_on.queue_ahead_count, cb_on.queue_ahead_volume, cb_on.prev_order_ids
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
