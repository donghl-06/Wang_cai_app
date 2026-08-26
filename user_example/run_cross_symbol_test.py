"""
旺财回测平台 - 跨标的订单路由验证

验证目标（本次新增功能：修复「串单」Bug）：
    - 策略在某个标的的 onTickEvent 里返回了「属于其它标的」的订单；
    - 修复前：订单被错挂到触发 tick 的那个引擎上（以错误的盘口、错误的价格精度撮合）；
    - 修复后：订单会暂存到 cross_symbol_events_，在 Taskflow join 之后由 MultiBacktestEngine
              按 symbol 路由到正确的子引擎，用正确的盘口撮合。

使用数据：
    300827.SZ @ 2025-11-17   open=45.34 元, upper_limit=54.84   (深交所股票,两位小数)
    510050.SH @ 2025-11-17   open=3.182 元, upper_limit=3.501   (上交所 ETF,三位小数)

策略思路：
    09:31:00 在 300827.SZ 的 onTickEvent 里**一次性**返回两笔订单：
      · 订单 X：买 300827.SZ 100 股 @ 50.00 元 (500000 厘)  ← 自己标的
      · 订单 Y：买 510050.SH 500 股 @  3.50 元  (35000 厘)  ← 跨标的

    两笔订单都设置为穿越对手价的价格,连续竞价撮合时应当各自成交。

    关键观察点（通过成交回调价格判断是否串单）：
      · 修复前：订单 Y 会在 300827.SZ 的 orderbook 上撮合。
                但 300827.SZ 的盘口在 45 元区间,Y 的价格 3.50 远远低于 bid1,
                不会立即成交。即使成交,也会出现「45 元成交 500 手」这种明显异常的数字。
      · 修复后：订单 Y 在 510050.SH 的 orderbook 上撮合,ask1 ≈ 3.182,
                Y 价格 3.50 >> ask1 → 立即成交 500 手,成交价 ≈ 3.18 元区间。

    → 只要订单 Y 的成交价落在 (2, 4) 元区间,就说明它确实走到了 510050.SH 引擎。

使用方法:
    python run_cross_symbol_test.py
"""

from typing import Dict, List
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
SYMBOL_A = "300827.SZ"   # 触发 tick 的标的（深市,两位小数）
SYMBOL_B = "510050.SH"   # 跨标的订单所属标的（沪市 ETF,三位小数）
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"

# 价格（厘）
PX_A_AGGRESSIVE = 500000   # 50.00 元，穿越 300827.SZ 的 ask1(≈45+)
PX_B_AGGRESSIVE = 35000    #  3.50 元，穿越 510050.SH 的 ask1(≈3.18+)

# 可接受的成交价区间（用于判断「是否串单」）
PX_A_RANGE_LI = (400000, 550000)   # 40 ~ 55 元
PX_B_RANGE_LI = (20000, 40000)     # 2 ~ 4 元
# ============================


def _fmt_time(t: int) -> str:
    t //= 1000
    return f"{t // 10000:02d}:{(t // 100) % 100:02d}:{t % 100:02d}"


class CrossSymbolRoutingStrategy(Strategy):
    """跨标的订单路由验证策略。"""

    def __init__(self, account: str = "cross_demo"):
        super().__init__()
        self.account = account

        self._submitted = False
        self.order_X = f"{account}_X_{SYMBOL_A}"  # 300827.SZ 本引擎订单
        self.order_Y = f"{account}_Y_{SYMBOL_B}"  # 510050.SH 跨标的订单
        self._target_orders = {self.order_X, self.order_Y}

        self._order_cbs: Dict[str, OrderCallback] = {}
        self._trades_by_order: Dict[str, List[dict]] = {}
        self._cancels_by_order: Dict[str, List[dict]] = {}

    def getStrategyId(self) -> str:
        return self.account

    # ================= 事件 =================

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        # 只在 A 标的的 tick 里触发（模拟「策略被 A 的 tick 驱动,却也给 B 下单」）
        if tick.Instrument != SYMBOL_A:
            return events

        tstr = _fmt_time(tick.Time)
        if self._submitted:
            return events
        if tick.Time < 93100000 or tstr < "09:31:00":
            return events

        # 09:31:00 及之后的第一次 A tick → 同时下 X(A) + Y(B)
        self._submitted = True
        events.append(make_order(
            Broker='', Account=self.account,
            Exchange=tick.Exchange,            # A 的 exchange
            Instrument=SYMBOL_A,
            OrderLocalID=self.order_X, OrderType=0,
            Direction=1, Price=PX_A_AGGRESSIVE, Volume=100,
        ))
        events.append(make_order(
            Broker='', Account=self.account,
            Exchange=0,                        # B 是 SH
            Instrument=SYMBOL_B,
            OrderLocalID=self.order_Y, OrderType=0,
            Direction=1, Price=PX_B_AGGRESSIVE, Volume=500,
        ))
        print(f"\n[{tstr}] 在 {SYMBOL_A} 的 tick 里同时下 2 笔单：")
        print(f"    X  买 {SYMBOL_A}  100 @ {PX_A_AGGRESSIVE/10000:.2f}  (本引擎)")
        print(f"    Y  买 {SYMBOL_B}  500 @ {PX_B_AGGRESSIVE/10000:.4f}  (跨标的 → 应被路由到 {SYMBOL_B} 引擎)")
        return events

    def onOrderEvent(self, order: OrderDetail):
        return []

    def onTradeEvent(self, trade: TradeDetail):
        return []

    # ================= 回调 =================

    def onOrderCallback(self, cb: OrderCallback) -> None:
        if cb.orderlocalid not in self._target_orders:
            return
        self._order_cbs[cb.orderlocalid] = cb
        side = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单回调: {cb.orderlocalid} {side} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.localid not in self._target_orders:
            return
        info = {
            "volume": cb.volume,
            "price": cb.price,
            "direction": cb.direction,
            "deltapos": cb.deltapos,
        }
        if cb.matchtype == 'T':
            self._trades_by_order.setdefault(cb.localid, []).append(info)
            side = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {cb.localid} {side} {cb.volume}@{cb.price/10000:.4f} "
                  f"(deltapos={cb.deltapos})")
        elif cb.matchtype == 'D':
            self._cancels_by_order.setdefault(cb.localid, []).append(info)
            print(f"  ☑️  撤单: {cb.localid}")

    # ================= 验证 =================

    def validate(self) -> bool:
        print(f"\n{'=' * 78}")
        print("跨标的订单路由功能验证")
        print(f"{'=' * 78}")
        ok = True

        # 1) 两笔订单都必须有下单回调
        print("\n[A] 下单回调到达情况（每笔都应有 1 次）")
        for oid in [self.order_X, self.order_Y]:
            has = oid in self._order_cbs
            print(f"   {'✅' if has else '❌'} {oid}: {'收到' if has else '未收到'}")
            ok = ok and has

        # 2) 两笔订单都应该成交
        x_fills = self._trades_by_order.get(self.order_X, [])
        y_fills = self._trades_by_order.get(self.order_Y, [])

        print(f"\n[B] 本引擎订单 X ({SYMBOL_A}) 预期成交")
        print(f"     成交笔数: {len(x_fills)}")
        if not x_fills:
            print("     ❌ 未成交")
            ok = False
        else:
            total_vol = sum(f["volume"] for f in x_fills)
            avg_px = sum(f["price"] * f["volume"] for f in x_fills) / max(total_vol, 1)
            print(f"     总量: {total_vol},  平均成交价: {avg_px/10000:.4f}")
            in_range = (PX_A_RANGE_LI[0] <= avg_px <= PX_A_RANGE_LI[1])
            status = "✅" if (total_vol == 100 and in_range) else "❌"
            print(f"     预期: 总量=100, 成交价 ∈ "
                  f"[{PX_A_RANGE_LI[0]/10000:.2f}, {PX_A_RANGE_LI[1]/10000:.2f}]  {status}")
            ok = ok and (total_vol == 100) and in_range

        print(f"\n[C] 跨标的订单 Y ({SYMBOL_B}) 路由验证（串单 bug 核心检查）")
        print(f"     成交笔数: {len(y_fills)}")
        if not y_fills:
            print(f"     ❌ 未成交 —— 可能意味着订单没有被路由到 {SYMBOL_B} 引擎")
            ok = False
        else:
            total_vol = sum(f["volume"] for f in y_fills)
            avg_px = sum(f["price"] * f["volume"] for f in y_fills) / max(total_vol, 1)
            print(f"     总量: {total_vol},  平均成交价: {avg_px/10000:.4f}")
            in_range = (PX_B_RANGE_LI[0] <= avg_px <= PX_B_RANGE_LI[1])
            status = "✅" if (total_vol > 0 and in_range) else "❌"
            print(f"     预期: 总量=500, 成交价 ∈ "
                  f"[{PX_B_RANGE_LI[0]/10000:.4f}, {PX_B_RANGE_LI[1]/10000:.4f}]  {status}")
            if not in_range:
                print(f"     ⚠️ 成交价不在 {SYMBOL_B} 合理区间 —— 强烈怀疑发生了串单！")
            ok = ok and in_range

        # 3) 两笔订单都不应该被撤
        print("\n[D] 不应出现撤单回调")
        for oid in [self.order_X, self.order_Y]:
            cs = self._cancels_by_order.get(oid, [])
            status = "✅" if not cs else "❌"
            print(f"   {status} {oid}: {len(cs)} 次撤单")
            ok = ok and (not cs)

        print(f"\n{'=' * 78}")
        print(f"  最终结论: {'✅ 全部通过' if ok else '❌ 存在未达预期的项'}")
        print(f"{'=' * 78}\n")
        return ok


# ================= 运行入口 =================

def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"✅ 数据加载完成: {symbol}")
    print(f"   Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main() -> int:
    print(f"\n{'=' * 78}")
    print("  跨标的订单路由 - 功能验证脚本")
    print(f"  标的: {SYMBOL_A} + {SYMBOL_B} @ {DATE}")
    print(f"{'=' * 78}")

    a_data = load_data(SYMBOL_A, DATE, DATA_DIR)
    b_data = load_data(SYMBOL_B, DATE, DATA_DIR)

    # 300827.SZ 是股票；510050.SH 是 ETF(使用 5 元组,is_etf=True)
    data = {
        SYMBOL_A: (*a_data, False),   # stock
        SYMBOL_B: (*b_data, True),    # etf
    }

    strategy = CrossSymbolRoutingStrategy(account="cross_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL_A}_{SYMBOL_B}_{DATE}_cross",
    )
    if not success:
        print("❌ 回测失败")
        return 1

    return 0 if strategy.validate() else 2


if __name__ == "__main__":
    raise SystemExit(main())
