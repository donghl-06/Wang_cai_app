"""
旺财回测平台 - 真实成交替代模式示例

功能说明:
    - real_trade_match_mode=True: 被动单只用真实成交量匹配
    - 主动单自动走严格模式（欠债限制）
    - 被动单需等排队量被真实成交消耗后才能成交

使用方法:
    python run_real_trade_mode.py
"""

import pandas as pd
from pathlib import Path
import wangcai_syn
from wangcai_syn import (
    Strategy, UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel,
)
from typing import List


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"
# ============================


class RealTradeModeStrategy(Strategy):
    """
    真实成交替代模式示例策略

    演示：
    - 9:31:00 下一笔被动买单（挂在 bid1 价位，等待真实成交消耗排队量后成交）
    - 9:32:00 下一笔主动买单（穿越价差，走严格模式，受可用量和欠债限制）
    """

    def __init__(self, account: str = "rt_demo"):
        super().__init__()
        self.account = account
        self._order_counter = 1000
        self._passive_placed = False
        self._passive_id = None
        self._aggressive_placed = False
        self._aggressive_id = None
        self.position = 0
        self.passive_filled = 0
        self.passive_fill_count = 0
        self.aggressive_filled = 0

    def getStrategyId(self) -> str:
        return self.account

    def _next_id(self, label: str) -> str:
        self._order_counter += 1
        return f"{self.account}_{label}_{self._order_counter}"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if tick.Time < 93000000:
            return events

        t = tick.Time // 1000
        second = t % 100; t //= 100
        minute = t % 100; hour = t // 100

        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        upper = getattr(tick, "UpperLimit", 0)

        # 被动买单：挂在 bid1，需排队等真实成交
        if not self._passive_placed and hour == 9 and minute == 31 and second == 0 and bid1 > 0:
            self._passive_id = self._next_id("passive_buy")
            events.append(make_order(
                Broker='', Account=self.account,
                Exchange=tick.Exchange, Instrument=tick.Instrument,
                OrderLocalID=self._passive_id, OrderType=0,
                Direction=1, Price=bid1, Volume=100,
            ))
            self._passive_placed = True
            print(f"\n[09:31:00] 下被动买单: {self._passive_id}")
            print(f"  价格: {bid1/10000:.4f} (bid1, 被动)")
            print(f"  数量: 100")
            print(f"  bid1排队量: {tick.bid_sizes[0]}")

        # 主动买单：穿越价差，走严格模式
        if not self._aggressive_placed and hour == 9 and minute == 32 and second == 0 and ask1 > 0:
            price = upper if upper > 0 else ask1
            self._aggressive_id = self._next_id("aggressive_buy")
            events.append(make_order(
                Broker='', Account=self.account,
                Exchange=tick.Exchange, Instrument=tick.Instrument,
                OrderLocalID=self._aggressive_id, OrderType=0,
                Direction=1, Price=price, Volume=500000,
            ))
            self._aggressive_placed = True
            print(f"\n[09:32:00] 下主动买单: {self._aggressive_id}")
            print(f"  价格: {price/10000:.4f} (穿越价差)")
            print(f"  数量: 500000 (大量，测试部分成交)")

        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.matchtype == 'T':
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            if self._passive_id and cb.localid == self._passive_id:
                self.passive_filled += cb.volume
                self.passive_fill_count += 1
                print(f"  [被动单成交] {cb.volume}@{cb.price/10000:.4f}"
                      f" (累计: {self.passive_filled}, 第{self.passive_fill_count}次)")
            elif self._aggressive_id and cb.localid == self._aggressive_id:
                self.aggressive_filled += cb.volume
                print(f"  [主动单成交] {cb.volume}@{cb.price/10000:.4f} (严格模式)")
            else:
                print(f"  [成交] {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f}")
        elif cb.matchtype == 'D':
            if self._aggressive_id and cb.localid == self._aggressive_id:
                print(f"  [主动单撤单] {cb.localid} (严格模式剩余撤单)")
            else:
                print(f"  [撤单] {cb.localid}")

    def onOrderFilled(self, order_id: str, price, volume) -> None:
        pass

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        pass

    def print_summary(self):
        print(f"\n{'='*60}")
        print(f"真实成交替代模式 - 回测摘要")
        print(f"{'='*60}")
        print(f"  最终持仓: {self.position}")
        print(f"  被动买单: 成交 {self.passive_filled} 股 ({self.passive_fill_count} 次)")
        print(f"  主动买单: 成交 {self.aggressive_filled} 股")
        print(f"{'='*60}")


def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"数据加载完成: {symbol}")
    print(f"  Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main():
    print(f"\n{'='*60}")
    print("真实成交替代模式示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print(f"  模式: real_trade_match_mode=True")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = RealTradeModeStrategy(account="rt_demo")

    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_rt_mode",
        real_trade_match_mode=True,
    )

    if success:
        strategy.print_summary()
    else:
        print("回测失败")
        return 1
    return 0


if __name__ == "__main__":
    exit(main())
