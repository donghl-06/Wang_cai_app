"""
旺财回测平台 - 实时合成 Tick 推送示例

功能说明:
    - 传入 realtime_tick_interval_ms(如 10/50/100)开启实时合成 Tick
    - 引擎每跨过一个间隔网格边界, 在当前市场事件处理完成后从内部
      订单簿合成一个十档 Snapshot, 触发 onRealTimeTickEvent 回调
    - 与官方 3 秒快照的区别:
        * 十档/最新价/涨跌停/委托总量来自订单簿实时状态(事件间不等待)
        * Volume/Turnover/NumTrades/High/Low 由引擎按重建的历史成交
          逐笔累计, 单调不减; 与官方快照的数值出入来自时间戳口径不同
          (官方 tick 有独立时间戳), 属正常现象, 不做对齐
    - 回测时间只随市场事件前进, 无事件的空白区间(如午休)不补发

使用方法:
    python run_realtime_tick_test.py
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
INTERVAL_MS = 100          # 合成 Tick 间隔(毫秒)
# ============================


class RealtimeTickStrategy(Strategy):
    """统计实时合成 Tick 数量并与官方 3 秒快照对比。"""

    def __init__(self, account: str = "rttick_demo"):
        super().__init__()
        self.account = account
        self.n_realtime = 0
        self.n_official = 0

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        self.n_official += 1
        return []

    def onRealTimeTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        self.n_realtime += 1
        if self.n_realtime == 1:
            bid1 = snapshot.bids[0] if snapshot.bids[0] > 0 else 0
            ask1 = snapshot.asks[0] if snapshot.asks[0] > 0 else 0
            print(f"  首个合成 Tick: 买一={bid1/10000:.4f} "
                  f"卖一={ask1/10000:.4f} 累计成交量={snapshot.Volume}")
        return []

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
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
    print("实时合成 Tick 推送示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print(f"  模式: realtime_tick_interval_ms={INTERVAL_MS}")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = RealtimeTickStrategy(account="rttick_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_rttick",
        realtime_tick_interval_ms=INTERVAL_MS,
    )
    if not success:
        print("❌ 回测失败")
        return 1

    print(f"\n✅ 实时合成 Tick 示例运行完成")
    print(f"   官方 3 秒快照回调: {strategy.n_official} 次")
    print(f"   合成 Tick 回调({INTERVAL_MS}ms 间隔): {strategy.n_realtime} 次")
    print("   说明: 合成 Tick 在事件处理间隙按间隔网格推送,")
    print("         盘中可拿到比 3 秒快照更及时的盘口状态")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
