"""
旺财回测平台 - 队列信息回调测试示例

功能说明:
    - 开启 queue_info_enabled=True
    - 在 onOrderCallback 中读取并验证队列字段：
        * queue_ahead_count
        * queue_ahead_volume
        * prev_order_ids

使用方法:
    python run_queue_info_test.py
"""

from typing import List, Optional
from pathlib import Path
import pandas as pd
import wangcai_syn
from wangcai_syn import (
    Strategy, UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order,
)


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"
# ============================


class QueueInfoTestStrategy(Strategy):
    """测试 onOrderCallback 队列字段的示例策略。"""

    def __init__(self, account: str = "queue_demo"):
        super().__init__()
        self.account = account
        self.placed = False
        self.order_id: Optional[str] = None
        self.order_cb: Optional[OrderCallback] = None
        self.position = 0

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if tick.Time < 93000000:
            return events

        # 09:31:00 下第一笔被动买单，便于观察“前方队列”字段
        t = tick.Time // 1000
        second = t % 100
        t //= 100
        minute = t % 100
        hour = t // 100

        if self.placed or not (hour == 9 and minute == 31 and second == 0):
            return events

        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        if bid1 <= 0:
            return events

        self.order_id = f"{self.account}_qinfo_001"
        self.placed = True
        events.append(make_order(
            Broker='',
            Account=self.account,
            Exchange=tick.Exchange,
            Instrument=tick.Instrument,
            OrderLocalID=self.order_id,
            OrderType=0,      # 限价单
            Direction=1,      # 买
            Price=bid1,       # 被动价
            Volume=100
        ))
        print(f"\n[09:31:00] 下单: {self.order_id}, price={bid1/10000:.4f}, vol=100")
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.matchtype == 'T':
            self.position = cb.deltapos

    def onOrderCallback(self, cb: OrderCallback) -> None:
        self.order_cb = cb
        print("  ✓ 收到下单回调:")
        print(f"    orderlocalid={cb.orderlocalid}, price={cb.price/10000:.4f}, volume={cb.volume}")
        print(f"    queue_ahead_count={cb.queue_ahead_count}")
        print(f"    queue_ahead_volume={cb.queue_ahead_volume}")
        print(f"    prev_order_ids={cb.prev_order_ids}")


def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"数据加载完成: {symbol}")
    print(f"  Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main() -> int:
    print(f"\n{'='*60}")
    print("队列信息回调测试示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print("  模式: queue_info_enabled=True")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = QueueInfoTestStrategy(account="queue_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_queue_info",
        queue_info_enabled=True,
    )
    if not success:
        print("❌ 回测失败")
        return 1

    if strategy.order_cb is None:
        print("❌ 测试失败：未收到 onOrderCallback")
        return 1

    cb = strategy.order_cb
    if cb.queue_ahead_count < 0 or cb.queue_ahead_volume < 0:
        print("❌ 测试失败：队列字段未正确填充（仍为默认值）")
        return 1
    if len(cb.prev_order_ids) > 3:
        print("❌ 测试失败：prev_order_ids 超过 3 个")
        return 1
    for oid in cb.prev_order_ids:
        if oid <= 0:
            print(f"❌ 测试失败：存在非法历史订单ID: {oid}")
            return 1

    print("\n✅ 队列信息回调测试通过")
    print("   说明：已保留原有回调字段，并额外返回队列字段")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
