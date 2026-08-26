"""
旺财回测平台 - 集合竞价影子订单验证

验证目标（本次新增功能）：
    1) 开盘集合竞价（09:15-09:25）期间下用户单 → 暂存为「影子单」，不进入真实集合竞价价格发现；
    2) settle（09:25）时：
        - 价格能穿越集合竞价价的订单 → 按集合竞价价全量成交，回调 matchtype='T'；
        - 价格不能穿越的订单     → 结转到连续竞价 (con_engine_->accept)，不撤单；
    3) 开盘集合竞价期间下单后立刻撤 → 直接从 pending_auction_orders_ 删除，
       回调 matchtype='D'，订单不会流入连续竞价；
    4) 收盘集合竞价（14:57-15:00）期间下用户单 → 同样暂存为「影子单」；
    5) settle（15:00）时：
        - 能穿越 → 'T' 成交；
        - 不能穿越 → 'D' 直接撤单（收盘不做结转）。

使用数据:
    300827.SZ @ 2025-11-17  (csbar1d:
        prevclose=45.70, open=45.34, upper_limit=54.84, lower_limit=36.56,
        high=46.30, low=43.56, close=44.44)

    选价说明:
        - 触价买价  = 50.00 元 (500000 厘)   高于 open/close,低于涨停;必成交
        - 非触价买价 = 40.00 元 (400000 厘)   低于当天 low=43.56,整日都吃不到;
                       开盘不触价 → 结转连续竞价(但仍不成交,留在盘中),
                       收盘不触价 → settle 后撤单

使用方法:
    python run_auction_shadow_order_test.py
"""

from typing import Dict, List
from pathlib import Path
import pandas as pd
import wangcai_syn
from wangcai_syn import (
    Strategy, UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel,
)


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../new_log"
OUTPUT_DIR = "./output"

# 选价（厘）
PX_CROSS = 500000     # 50.00 元，高于 open=45.34 且 close=44.44,能穿越集合竞价价
PX_NON_CROSS = 400000  # 40.00 元，低于当天 low=43.56,不能穿越
# ============================


def _fmt_time(t: int) -> str:
    """tick.Time 是 HHMMSSMMM 形式的整数"""
    t //= 1000
    return f"{t // 10000:02d}:{(t // 100) % 100:02d}:{t % 100:02d}"


class AuctionShadowOrderStrategy(Strategy):
    """在开盘/收盘集合竞价期间下影子单，验证新增的 settleAuctionUserOrders 行为。"""

    def __init__(self, account: str = "auction_demo"):
        super().__init__()
        self.account = account

        self._submitted_open = False
        self._cancel_sent = False
        self._submitted_close = False

        self.order_A = f"{account}_A_open_cross"     # 开盘触价 → 预期 T 成交
        self.order_B = f"{account}_B_open_noncross"  # 开盘不触价 → 预期结转连续竞价
        self.order_C = f"{account}_C_open_cancel"    # 开盘触价,09:22 撤 → 预期 D 撤单
        self.order_D = f"{account}_D_close_cross"    # 收盘触价 → 预期 T 成交
        self.order_E = f"{account}_E_close_noncross" # 收盘不触价 → 预期 D 撤单

        self._target_orders = {
            self.order_A, self.order_B, self.order_C,
            self.order_D, self.order_E,
        }

        self._oc_count: Dict[str, int] = {}                    # orderlocalid → OrderCallback 次数
        self._trades_by_order: Dict[str, List[dict]] = {}      # orderlocalid → 成交明细
        self._cancels_by_order: Dict[str, List[dict]] = {}     # orderlocalid → 撤单明细

    def getStrategyId(self) -> str:
        return self.account

    # ================= 事件 =================

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        tstr = _fmt_time(tick.Time)

        # 开盘集合竞价期间(09:15-09:25),选 09:20:00 作为触发点
        if (not self._submitted_open
                and tick.Time >= 91500000 and tick.Time < 92500000
                and tstr >= "09:20:00"):
            self._submitted_open = True
            events += [
                make_order(Broker='', Account=self.account,
                           Exchange=tick.Exchange, Instrument=tick.Instrument,
                           OrderLocalID=self.order_A, OrderType=0,
                           Direction=1, Price=PX_CROSS, Volume=100),
                make_order(Broker='', Account=self.account,
                           Exchange=tick.Exchange, Instrument=tick.Instrument,
                           OrderLocalID=self.order_B, OrderType=0,
                           Direction=1, Price=PX_NON_CROSS, Volume=100),
                make_order(Broker='', Account=self.account,
                           Exchange=tick.Exchange, Instrument=tick.Instrument,
                           OrderLocalID=self.order_C, OrderType=0,
                           Direction=1, Price=PX_CROSS, Volume=200),
            ]
            print(f"\n[{tstr}] 开盘集合竞价影子单 ×3：")
            print(f"    A({self.order_A})  买 100 @ {PX_CROSS/10000:.2f}  (穿越→应成交)")
            print(f"    B({self.order_B})  买 100 @ {PX_NON_CROSS/10000:.2f}  (非穿越→应结转连续竞价)")
            print(f"    C({self.order_C})  买 200 @ {PX_CROSS/10000:.2f}  (稍后撤单)")
            return events

        # 09:22:00 撤 C(仍在集合竞价期间)
        if (self._submitted_open and not self._cancel_sent
                and tick.Time >= 91500000 and tick.Time < 92500000
                and tstr >= "09:22:00"):
            self._cancel_sent = True
            events.append(make_cancel(
                Broker='', Account=self.account,
                Exchange=tick.Exchange, Instrument=tick.Instrument,
                CancelOrderLocalID=f"{self.order_C}_cancel",
                OrderLocalID=self.order_C,
            ))
            print(f"\n[{tstr}] 集合竞价期间撤单 C：{self.order_C}")
            return events

        # 收盘集合竞价期间(14:57-15:00),选 14:58:00 作为触发点
        if (not self._submitted_close
                and tick.Time >= 145700000 and tick.Time < 150000000
                and tstr >= "14:58:00"):
            self._submitted_close = True
            events += [
                make_order(Broker='', Account=self.account,
                           Exchange=tick.Exchange, Instrument=tick.Instrument,
                           OrderLocalID=self.order_D, OrderType=0,
                           Direction=1, Price=PX_CROSS, Volume=100),
                make_order(Broker='', Account=self.account,
                           Exchange=tick.Exchange, Instrument=tick.Instrument,
                           OrderLocalID=self.order_E, OrderType=0,
                           Direction=1, Price=PX_NON_CROSS, Volume=100),
            ]
            print(f"\n[{tstr}] 收盘集合竞价影子单 ×2：")
            print(f"    D({self.order_D})  买 100 @ {PX_CROSS/10000:.2f}  (穿越→应成交)")
            print(f"    E({self.order_E})  买 100 @ {PX_NON_CROSS/10000:.2f}  (非穿越→应撤单)")
            return events

        return events

    def onOrderEvent(self, order: OrderDetail):
        return []

    def onTradeEvent(self, trade: TradeDetail):
        return []

    # ================= 回调 =================

    def onOrderCallback(self, cb: OrderCallback) -> None:
        if cb.orderlocalid not in self._target_orders:
            return
        self._oc_count[cb.orderlocalid] = self._oc_count.get(cb.orderlocalid, 0) + 1
        side = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单回调: {cb.orderlocalid} {side} {cb.volume}@{cb.price/10000:.2f}")

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
        print("集合竞价影子订单功能验证")
        print(f"{'=' * 78}")
        ok = True

        # 1) 每笔下单都必须恰好触发 1 次 OrderCallback
        #    （旧逻辑在集合竞价阶段可能直接拒单并跳过下单回调,不应该再出现）
        print("\n[A] 下单回调到达情况（每笔都应有 1 次）")
        for oid in [self.order_A, self.order_B, self.order_C, self.order_D, self.order_E]:
            got = self._oc_count.get(oid, 0)
            status = "✅" if got == 1 else "❌"
            print(f"   {status} {oid}: {got} 次")
            ok = ok and (got == 1)

        # 2) 开盘触价买 A：1 次成交(volume=100), 0 次撤单
        A_fills = self._trades_by_order.get(self.order_A, [])
        A_cancels = self._cancels_by_order.get(self.order_A, [])
        ok_A = (len(A_fills) == 1 and A_fills[0]["volume"] == 100 and not A_cancels)
        print("\n[B] 开盘集合竞价 A：预期 1 成交 100 手 + 0 撤单")
        print(f"     实际成交 {len(A_fills)} 笔: {A_fills}")
        print(f"     实际撤单 {len(A_cancels)} 笔")
        print(f"     {'✅ 通过' if ok_A else '❌ 未通过'}")
        ok = ok and ok_A

        # 3) 开盘不触价买 B：不应该撤单(应结转到连续竞价;40 元低于日内 low,不成交)
        B_fills = self._trades_by_order.get(self.order_B, [])
        B_cancels = self._cancels_by_order.get(self.order_B, [])
        ok_B = (not B_cancels)  # 结转后不强制必须成交或不成交
        print("\n[C] 开盘集合竞价 B：预期结转连续竞价（无撤单；40 元偏低,整日很可能未成交）")
        print(f"     实际成交 {len(B_fills)} 笔，实际撤单 {len(B_cancels)} 笔")
        print(f"     {'✅ 通过' if ok_B else '❌ 未通过(收到了非预期的撤单回调)'}")
        ok = ok and ok_B

        # 4) 开盘期间撤单的 C：0 次成交, 1 次撤单
        C_fills = self._trades_by_order.get(self.order_C, [])
        C_cancels = self._cancels_by_order.get(self.order_C, [])
        ok_C = (not C_fills and len(C_cancels) == 1)
        print("\n[D] 开盘集合竞价 C（09:22 主动撤）：预期 0 成交 + 1 撤单")
        print(f"     实际成交 {len(C_fills)} 笔,实际撤单 {len(C_cancels)} 笔")
        print(f"     {'✅ 通过' if ok_C else '❌ 未通过'}")
        ok = ok and ok_C

        # 5) 收盘触价买 D：1 次成交(volume=100), 0 次撤单
        D_fills = self._trades_by_order.get(self.order_D, [])
        D_cancels = self._cancels_by_order.get(self.order_D, [])
        ok_D = (len(D_fills) == 1 and D_fills[0]["volume"] == 100 and not D_cancels)
        print("\n[E] 收盘集合竞价 D：预期 1 成交 100 手 + 0 撤单")
        print(f"     实际成交 {len(D_fills)} 笔：{D_fills}")
        print(f"     实际撤单 {len(D_cancels)} 笔")
        print(f"     {'✅ 通过' if ok_D else '❌ 未通过'}")
        ok = ok and ok_D

        # 6) 收盘不触价买 E：0 次成交, 1 次撤单(settle 时自动撤)
        E_fills = self._trades_by_order.get(self.order_E, [])
        E_cancels = self._cancels_by_order.get(self.order_E, [])
        ok_E = (not E_fills and len(E_cancels) == 1)
        print("\n[F] 收盘集合竞价 E：预期 0 成交 + 1 撤单（settle 时自动撤）")
        print(f"     实际成交 {len(E_fills)} 笔,实际撤单 {len(E_cancels)} 笔")
        print(f"     {'✅ 通过' if ok_E else '❌ 未通过'}")
        ok = ok and ok_E

        print(f"\n{'=' * 78}")
        print(f"  最终结论：{'✅ 全部通过' if ok else '❌ 存在未达预期的项'}")
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
    print("  集合竞价影子订单 - 功能验证脚本")
    print(f"  标的: {SYMBOL} @ {DATE}")
    print(f"{'=' * 78}")

    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    data = {SYMBOL: (cstick, csord, cstra, csbar1d)}

    strategy = AuctionShadowOrderStrategy(account="auction_demo")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_auction_shadow",
    )
    if not success:
        print("❌ 回测失败")
        return 1

    return 0 if strategy.validate() else 2


if __name__ == "__main__":
    raise SystemExit(main())
