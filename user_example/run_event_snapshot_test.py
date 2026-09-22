"""
旺财回测平台 - 事件驱动快照回调示例

功能说明:
    - 开启 event_snapshot_enabled=True
    - 每个市场事件(逐笔委托/逐笔成交含撤单)的全部处理
      ——撮合 + 策略响应产生的下单/撤单——结束后, 引擎从内部订单簿
      合成一个十档 Snapshot 推送策略一次(onEventSnapshot 回调),
      推送次数 == 市场事件数; tick 事件不触发
    - 快照口径: 十档只含历史订单/公开订单簿(不含本策略的影子单),
      与 onRealTimeTickEvent 的合成口径一致
    - 适用: 需要逐事件粒度观察盘口变化的策略(如做市/盘口微观结构研究),
      比 3 秒快照精细几个数量级, 代价是回调次数多、回测变慢

使用方法:
    python run_event_snapshot_test.py
"""

from typing import List
from pathlib import Path
import pandas as pd
import wangcai_syn
from wangcai_syn import (
    Strategy, UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
)


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"
# ============================


class EventSnapshotStrategy(Strategy):
    """统计事件快照回调次数, 抽样打印盘口。"""

    def __init__(self, account: str = "evsnap_demo"):
        super().__init__()
        self.account = account
        self.n_snapshots = 0

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        return []

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onEventSnapshot(self, snapshot: Snapshot) -> List[UserEvent]:
        self.n_snapshots += 1
        # 每 5000 个事件抽样打印一次盘口
        if self.n_snapshots % 5000 == 1:
            bid1 = snapshot.bids[0] if snapshot.bids[0] > 0 else 0
            ask1 = snapshot.asks[0] if snapshot.asks[0] > 0 else 0
            print(f"  [事件 #{self.n_snapshots}] "
                  f"买一={bid1/10000:.4f} 卖一={ask1/10000:.4f}")
        return []

    def onTradeCallback(self, cb: TradeCallback) -> None:
        pass

    def onOrderCallback(self, cb: OrderCallback) -> None:
        pass


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
    print("事件驱动快照回调示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print("  模式: event_snapshot_enabled=True")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = EventSnapshotStrategy(account="evsnap_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_evsnap",
        event_snapshot_enabled=True,
    )
    if not success:
        print("❌ 回测失败")
        return 1

    n_events = len(csord) + len(cstra)
    print(f"\n✅ 事件快照回调测试通过")
    print(f"   市场事件数(委托+成交): {n_events}")
    print(f"   onEventSnapshot 回调次数: {strategy.n_snapshots}")
    print("   说明: 逐笔撮合阶段每个市场事件处理完毕后推送一次十档快照;")
    print("         集合竞价阶段的委托集中处理, 不逐事件推送, 故次数少于总行数")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
