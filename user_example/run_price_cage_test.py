"""
旺财回测平台 - 策略单价格笼子示例

功能说明:
    - 价格笼子(有效申报价格范围)默认开启: user_cage_enabled=True
    - 规则按数据日期×板块自动判定:
        * 主板: 2023-04-10 起 ±2% 与 0.1 元孰高, 超范围废单
        * 创业板: 2020-08-24 至 2023-04-10 暂存模式(超范围入笼,
          价格落回范围自动恢复参与撮合); 之后与主板相同
        * 科创板: 2019-07-22 开市起拒单(纯 ±2%), 2023-04-10 后加 0.1 元兜底
        * 北交所/ETF/债券等: 不适用
    - 有效申报范围 = 基准价±2% 四舍五入至最小变动价位
      (基准价链: 对手一档→本方一档→最新成交→昨收)
    - 另: 策略限价单有涨跌停前置校验(超涨跌停废单, 所有时代)
    - 本示例(创业板, 2025 年, 拒单模式): 同时下一笔笼内单和一笔
      笼外单。两笔都会先收到下单确认(申报回执), 随后笼外单被废单——
      引擎日志打印"[虚拟撤单] ... 原因: 价格笼子：...废单", 策略侧
      通过 onTradeCallback 收到 matchtype='D' 的回调; 笼内单正常留簿
使用方法:
    python run_price_cage_test.py
"""

from typing import List
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


class PriceCageTestStrategy(Strategy):
    """09:31:00 同时下笼内/笼外两笔买单, 对比结局。"""

    def __init__(self, account: str = "cage_demo"):
        super().__init__()
        self.account = account
        self.placed = False
        self.accepted: List[str] = []
        self.cancelled: List[str] = []   # 收到 'D' 回调的订单(本例中=废单)

    def getStrategyId(self) -> str:
        return self.account

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if self.placed or tick.Time < 93100000:
            return events

        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        if bid1 <= 0 or ask1 <= 0:
            return events

        self.placed = True

        # 笼内单: 买一价, 必在有效申报范围内
        events.append(make_order(
            Broker='', Account=self.account,
            Exchange=tick.Exchange, Instrument=tick.Instrument,
            OrderLocalID=f"{self.account}_in_cage",
            OrderType=0, Direction=1, Price=bid1, Volume=100
        ))

        # 笼外单: 卖一价 +5%, 远超 +2% 笼子上沿(对齐到 0.01 元=100 厘)
        out_price = int(ask1 * 1.05) // 100 * 100
        events.append(make_order(
            Broker='', Account=self.account,
            Exchange=tick.Exchange, Instrument=tick.Instrument,
            OrderLocalID=f"{self.account}_out_cage",
            OrderType=0, Direction=1, Price=out_price, Volume=100
        ))

        print(f"\n[09:31:00] 买一={bid1/10000:.4f} 卖一={ask1/10000:.4f}")
        print(f"  笼内单: {bid1/10000:.4f} x 100 (预期: 接受)")
        print(f"  笼外单: {out_price/10000:.4f} x 100 (预期: 废单, "
              f"超 +2% 笼子上沿)")
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onTradeCallback(self, cb: TradeCallback) -> None:
        # 废单走 matchtype='D' 回调(引擎日志同时打印废单原因);
        # 本例策略从不主动撤单, 故任何 'D' 都是拒单废单
        if cb.matchtype == 'D':
            self.cancelled.append(cb.localid)
            print(f"  ✗ 废单通知: {cb.localid} (matchtype='D')")

    def onOrderCallback(self, cb: OrderCallback) -> None:
        self.accepted.append(cb.orderlocalid)
        print(f"  ✓ 下单确认: {cb.orderlocalid} "
              f"price={cb.price/10000:.4f} volume={cb.volume}")


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
    print("策略单价格笼子示例")
    print(f"{'='*60}")
    print(f"  标的: {SYMBOL} @ {DATE} (创业板, 注册制后: 笼外废单)")
    print("  模式: user_cage_enabled=True(默认)")
    print(f"{'='*60}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = PriceCageTestStrategy(account="cage_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_cage",
        user_cage_enabled=True,   # 默认值, 显式写出以示说明; False 可关闭
    )
    if not success:
        print("❌ 回测失败")
        return 1

    in_cage_ok = any("in_cage" in oid for oid in strategy.accepted)
    out_cage_ok = any("out_cage" in oid for oid in strategy.accepted)
    out_cage_rejected = any("out_cage" in oid for oid in strategy.cancelled)
    in_cage_cancelled = any("in_cage" in oid for oid in strategy.cancelled)

    if not in_cage_ok:
        print("❌ 测试失败: 笼内单未收到下单确认")
        return 1
    if not out_cage_ok:
        print("❌ 测试失败: 笼外单应先收到申报回执(随后才被废单)")
        return 1
    if not out_cage_rejected:
        print("❌ 测试失败: 笼外单未收到废单回调(matchtype='D')")
        return 1
    if in_cage_cancelled:
        print("❌ 测试失败: 笼内单不应被废单")
        return 1

    print("\n✅ 价格笼子示例运行完成")
    print(f"   笼内单正常留簿: {[o for o in strategy.accepted if 'in_cage' in o]}")
    print(f"   笼外单已废单: {[o for o in strategy.cancelled]}")
    print("   说明: user_cage_enabled=False 可关闭策略单笼子判定,")
    print("         但历史订单簿侧的笼子语义(交易所行为)始终生效")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
