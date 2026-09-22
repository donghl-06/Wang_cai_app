"""
旺财回测平台 - 下单延迟模拟示例

功能说明:
    - 开启 order_latency_enabled=True, 模拟策略到交易所的链路时延
    - 策略下单/撤单不再立即进簿, 而是延迟到 发出时刻+latency 才"到达交易所":
      过涨跌停/价格笼子校验、进订单簿、参与撮合; 下单确认回调同样延迟发出
    - order_latency_ms: 延迟时长, 自动四舍五入对齐到市场最小事件粒度 10ms
      (14→10, 15→20), 开启状态下最小 10ms; 开启但未指定时默认 20ms
    - latency_entry_position: 多笔订单同一时刻到达时, 本策略单排在
      同时间订单的头部("head", 默认, 抢排队优先级)还是尾部("tail")
    - 撤单与下单走同一延迟通道(保 FIFO); 收盘(15:00)后才到达的订单被丢弃
    - 也可在策略 __init__ 里 self.setOrderLatencyMs(n) /
      self.setLatencyEntryPosition(LatencyEntryPosition.Tail), 效果相同

使用方法:
    python run_order_latency_test.py
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
LATENCY_MS = 50            # 延迟 50ms(10 的倍数, 无需对齐)
# ============================


def fmt_time(t) -> str:
    """HHMMSSmmm 整数 -> 可读字符串; 无法解析时原样返回。"""
    try:
        t = int(t)
        ms = t % 1000
        s = (t // 1000) % 100
        m = (t // 100000) % 100
        h = t // 10000000
        return f"{h:02d}:{m:02d}:{s:02d}.{ms:03d}"
    except (TypeError, ValueError):
        return str(t)


class LatencyTestStrategy(Strategy):
    """09:31:00 下一笔限价买单, 观察下单确认回调的到达时刻。"""

    def __init__(self, account: str = "latency_demo"):
        super().__init__()
        self.account = account
        self.placed = False
        self.place_time: Optional[int] = None   # 策略发出订单的市场时刻
        self.confirm_time = None                # 下单确认回调的时刻

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if self.placed or tick.Time < 93100000:
            return events

        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        if bid1 <= 0:
            return events

        self.placed = True
        self.place_time = tick.Time
        events.append(make_order(
            Broker='',
            Account=self.account,
            Exchange=tick.Exchange,
            Instrument=tick.Instrument,
            OrderLocalID=f"{self.account}_lat_001",
            OrderType=0,      # 限价单
            Direction=1,      # 买
            Price=bid1,
            Volume=100
        ))
        print(f"\n[{fmt_time(tick.Time)}] 策略发出订单(被动买 {bid1/10000:.4f} x 100)")
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTradeCallback(self, cb: TradeCallback) -> None:
        pass

    def onOrderCallback(self, cb: OrderCallback) -> None:
        self.confirm_time = cb.time
        print(f"[{fmt_time(cb.time)}] ✓ 收到下单确认回调: {cb.orderlocalid}")
        if self.place_time is not None:
            try:
                delay = int(cb.time) - int(self.place_time)
                print(f"   链路延迟约 {delay / 1000:.0f}ms "
                      f"(订单延迟进簿, 确认回调同步延迟)")
            except (TypeError, ValueError):
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
    print("下单延迟模拟示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print(f"  模式: order_latency_enabled=True, latency={LATENCY_MS}ms, head")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = LatencyTestStrategy(account="latency_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_latency",
        order_latency_enabled=True,
        order_latency_ms=LATENCY_MS,
        latency_entry_position="head",
    )
    if not success:
        print("❌ 回测失败")
        return 1

    if strategy.confirm_time is None:
        print("❌ 测试失败: 未收到下单确认回调")
        return 1

    print("\n✅ 下单延迟示例运行完成")
    print("   说明: 开启延迟后, 订单在 发出时刻+latency 才到达交易所,")
    print("         高频策略的排队位置与成交结果会与零延迟回测不同")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
