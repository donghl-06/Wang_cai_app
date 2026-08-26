"""
真实成交替代模式（RT）覆盖测试套件

覆盖重点：
1) RT 开启时，主动单自动走严格模式（欠债限制生效）
2) RT 关闭时，主动单不受上述限制（基线对照）
3) RT 被动单双边撤单路径（RT 虚拟订单撤单）
4) RT 被动单不应在下单同秒被历史成交池“倒灌”成交（pool_at_entry 基线）
"""

from typing import Dict, List, Optional, Tuple

from wangcai_syn import (
    Strategy,
    UserEvent,
    Snapshot,
    OrderDetail,
    TradeDetail,
    TradeCallback,
    OrderCallback,
    make_order,
    make_cancel,
)


def _tick_seconds(tick_time: int) -> int:
    t = tick_time // 1000
    sec = t % 100
    t = t // 100
    minute = t % 100
    hour = t // 100
    return hour * 3600 + minute * 60 + sec


def _datetime_seconds(datetime_str: str) -> int:
    if len(datetime_str) < 19:
        return -1
    hh = int(datetime_str[11:13])
    mm = int(datetime_str[14:16])
    ss = int(datetime_str[17:19])
    return hh * 3600 + mm * 60 + ss


class _ScenarioBase(Strategy):
    def __init__(self, account: str):
        super().__init__()
        self.account = account
        self._order_counter = 5000
        self.order_trades: Dict[str, int] = {}
        self.order_cancels: Dict[str, int] = {}
        self.order_submits: Dict[str, int] = {}

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self, label: str) -> str:
        self._order_counter += 1
        oid = f"{self.account}_{label}_{self._order_counter}"
        self.order_trades[oid] = 0
        self.order_cancels[oid] = 0
        self.order_submits[oid] = 0
        return oid

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderFilled(self, order_id: str, price, volume) -> None:
        pass

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        pass

    def onOrderCallback(self, cb: OrderCallback) -> None:
        self.order_submits[cb.orderlocalid] = self.order_submits.get(cb.orderlocalid, 0) + 1

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.matchtype == "T":
            self.order_trades[cb.localid] = self.order_trades.get(cb.localid, 0) + int(cb.volume)
            self._on_trade(cb)
        elif cb.matchtype == "D":
            self.order_cancels[cb.localid] = self.order_cancels.get(cb.localid, 0) + 1
            self._on_cancel(cb)

    def _on_trade(self, cb: TradeCallback) -> None:
        pass

    def _on_cancel(self, cb: TradeCallback) -> None:
        pass

    def validate(self) -> Tuple[bool, List[str]]:
        return True, ["[PASS] base validate"]


class RtStrictBridgeScenario(_ScenarioBase):
    """RT开启时，主动单应复用严格主动单模式。"""

    def __init__(self, account: str):
        super().__init__(account)
        self.placed = False
        self.first_id: Optional[str] = None
        self.second_id: Optional[str] = None
        self.second_cancelled = False

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if self.placed or tick.Time < 93100000:
            return events

        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        upper = getattr(tick, "UpperLimit", 0)
        if ask1 <= 0 or upper <= 0:
            return events

        self.first_id = self._next_order_id("rt_strict_first")
        self.second_id = self._next_order_id("rt_strict_second")

        events.append(
            make_order(
                Broker="",
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=self.first_id,
                OrderType=0,
                Direction=1,
                Price=upper,
                Volume=300000,
            )
        )
        events.append(
            make_order(
                Broker="",
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=self.second_id,
                OrderType=0,
                Direction=1,
                Price=upper,
                Volume=100,
            )
        )
        self.placed = True
        return events

    def _on_cancel(self, cb: TradeCallback) -> None:
        if self.second_id and cb.localid == self.second_id:
            self.second_cancelled = True

    def validate(self) -> Tuple[bool, List[str]]:
        msgs: List[str] = []
        passed = True

        if not self.placed or not self.first_id or not self.second_id:
            return False, ["[FAIL] 未成功下发两笔主动单，无法验证RT->严格模式联动"]

        first_trade = self.order_trades.get(self.first_id, 0)
        second_trade = self.order_trades.get(self.second_id, 0)

        if first_trade > 0:
            msgs.append(f"[PASS] 第一笔主动单有成交: {first_trade}")
        else:
            passed = False
            msgs.append("[FAIL] 第一笔主动单未成交，无法构造欠债场景")

        if second_trade == 0 and self.second_cancelled:
            msgs.append("[PASS] 第二笔主动单被拦截（无成交且收到D回报）")
        else:
            passed = False
            msgs.append(
                f"[FAIL] 第二笔主动单未按严格模式被拦截: trade={second_trade}, cancelled={self.second_cancelled}"
            )

        return passed, msgs


class RtOffBaselineScenario(_ScenarioBase):
    """RT关闭基线：同样两笔主动单不应被“RT严格联动”拦截。"""

    def __init__(self, account: str):
        super().__init__(account)
        self.placed = False
        self.first_id: Optional[str] = None
        self.second_id: Optional[str] = None
        self.second_cancelled = False

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if self.placed or tick.Time < 93100000:
            return events

        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        upper = getattr(tick, "UpperLimit", 0)
        if ask1 <= 0 or upper <= 0:
            return events

        self.first_id = self._next_order_id("rt_off_first")
        self.second_id = self._next_order_id("rt_off_second")

        events.append(
            make_order(
                Broker="",
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=self.first_id,
                OrderType=0,
                Direction=1,
                Price=upper,
                Volume=200000,
            )
        )
        events.append(
            make_order(
                Broker="",
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=self.second_id,
                OrderType=0,
                Direction=1,
                Price=upper,
                Volume=100,
            )
        )
        self.placed = True
        return events

    def _on_cancel(self, cb: TradeCallback) -> None:
        if self.second_id and cb.localid == self.second_id:
            self.second_cancelled = True

    def validate(self) -> Tuple[bool, List[str]]:
        if not self.placed or not self.first_id or not self.second_id:
            return False, ["[FAIL] 基线场景未成功下发两笔主动单"]

        first_trade = self.order_trades.get(self.first_id, 0)
        second_trade = self.order_trades.get(self.second_id, 0)
        passed = first_trade > 0 and second_trade > 0 and (not self.second_cancelled)

        msgs = [
            f"[INFO] first_trade={first_trade}",
            f"[INFO] second_trade={second_trade}",
            f"[INFO] second_cancelled={self.second_cancelled}",
        ]
        if passed:
            msgs.append("[PASS] RT关闭基线符合预期：两笔主动单都可成交，第二笔未被拦截")
        else:
            msgs.append("[FAIL] RT关闭基线异常：第二笔出现拦截或未成交")
        return passed, msgs


class RtPassiveCancelBothSidesScenario(_ScenarioBase):
    """RT被动单双边撤单路径：验证 RT 虚拟订单撤单分支可用。"""

    def __init__(self, account: str):
        super().__init__(account)
        self.step = 0
        self.buy_id: Optional[str] = None
        self.sell_id: Optional[str] = None
        self.buy_place_sec = -1
        self.sell_place_sec = -1
        self.buy_cancel_sent = False
        self.sell_cancel_sent = False
        self.last_exchange = 1
        self.last_instrument = ""

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if tick.Time < 93100000:
            return events

        sec = _tick_seconds(tick.Time)
        self.last_exchange = tick.Exchange
        self.last_instrument = tick.Instrument
        lower = getattr(tick, "LowerLimit", 0)
        upper = getattr(tick, "UpperLimit", 0)
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0

        if self.step == 0:
            buy_px = lower if lower > 0 else bid1
            if buy_px > 0:
                self.buy_id = self._next_order_id("rt_passive_buy")
                events.append(
                    make_order(
                        Broker="",
                        Account=self.account,
                        Exchange=tick.Exchange,
                        Instrument=tick.Instrument,
                        OrderLocalID=self.buy_id,
                        OrderType=0,
                        Direction=1,
                        Price=buy_px,
                        Volume=100,
                    )
                )
                self.buy_place_sec = sec
                self.step = 1
            return events

        if self.step == 1:
            if self.buy_id and (not self.buy_cancel_sent) and sec > self.buy_place_sec:
                events.append(
                    make_cancel(
                        Broker="",
                        Account=self.account,
                        Exchange=self.last_exchange,
                        Instrument=self.last_instrument,
                        CancelOrderLocalID=self._next_order_id("cancel_buy_req"),
                        OrderLocalID=self.buy_id,
                    )
                )
                self.buy_cancel_sent = True
            sell_px = upper if upper > 0 else ask1
            if self.buy_cancel_sent and self.sell_id is None and sell_px > 0 and sec > self.buy_place_sec:
                self.sell_id = self._next_order_id("rt_passive_sell")
                events.append(
                    make_order(
                        Broker="",
                        Account=self.account,
                        Exchange=tick.Exchange,
                        Instrument=tick.Instrument,
                        OrderLocalID=self.sell_id,
                        OrderType=0,
                        Direction=2,
                        Price=sell_px,
                        Volume=100,
                    )
                )
                self.sell_place_sec = sec
                self.step = 2
            return events

        if self.step == 2:
            if self.sell_id and (not self.sell_cancel_sent) and sec > self.sell_place_sec:
                events.append(
                    make_cancel(
                        Broker="",
                        Account=self.account,
                        Exchange=self.last_exchange,
                        Instrument=self.last_instrument,
                        CancelOrderLocalID=self._next_order_id("cancel_sell_req"),
                        OrderLocalID=self.sell_id,
                    )
                )
                self.sell_cancel_sent = True
                self.step = 3
            return events

        return events

    def validate(self) -> Tuple[bool, List[str]]:
        msgs: List[str] = []
        passed = True

        if not self.buy_id or not self.sell_id:
            return False, ["[FAIL] 被动单双边场景下单未完整执行"]

        buy_cancelled = self.order_cancels.get(self.buy_id, 0) > 0
        sell_cancelled = self.order_cancels.get(self.sell_id, 0) > 0
        buy_trade = self.order_trades.get(self.buy_id, 0)
        sell_trade = self.order_trades.get(self.sell_id, 0)

        if buy_cancelled and sell_cancelled:
            msgs.append("[PASS] 被动买卖单均收到撤单回报（RT撤单路径可用）")
        else:
            passed = False
            msgs.append(f"[FAIL] 撤单回报异常: buy_cancelled={buy_cancelled}, sell_cancelled={sell_cancelled}")

        if buy_trade == 0 and sell_trade == 0:
            msgs.append("[PASS] 双边被动单在短窗口内未异常成交")
        else:
            passed = False
            msgs.append(f"[FAIL] 被动单出现非预期成交: buy_trade={buy_trade}, sell_trade={sell_trade}")

        return passed, msgs


class RtPassiveFillTimingScenario(_ScenarioBase):
    """若被动单发生成交，首笔成交时间必须晚于下单时间（不允许同秒倒灌）。"""

    def __init__(self, account: str):
        super().__init__(account)
        self.order_id: Optional[str] = None
        self.place_sec: int = -1
        self.first_fill_sec: int = -1
        self.cancel_sent = False
        self.last_exchange = 1
        self.last_instrument = ""

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events: List[UserEvent] = []
        if tick.Time < 93100000:
            return events

        sec = _tick_seconds(tick.Time)
        self.last_exchange = tick.Exchange
        self.last_instrument = tick.Instrument
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0

        if self.order_id is None and bid1 > 0:
            self.order_id = self._next_order_id("rt_timing_buy")
            self.place_sec = sec
            events.append(
                make_order(
                    Broker="",
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=self.order_id,
                    OrderType=0,
                    Direction=1,
                    Price=bid1,   # 最优买一，易覆盖排队路径
                    Volume=100,
                )
            )
            return events

        # 防止长期挂单影响收盘，120秒后主动撤单
        if self.order_id and (not self.cancel_sent) and self.place_sec >= 0 and sec >= self.place_sec + 120:
            events.append(
                make_cancel(
                    Broker="",
                    Account=self.account,
                    Exchange=self.last_exchange,
                    Instrument=self.last_instrument,
                    CancelOrderLocalID=self._next_order_id("cancel_timing_req"),
                    OrderLocalID=self.order_id,
                )
            )
            self.cancel_sent = True
        return events

    def _on_trade(self, cb: TradeCallback) -> None:
        if self.order_id and cb.localid == self.order_id and self.first_fill_sec < 0:
            self.first_fill_sec = _datetime_seconds(cb.matchtime)

    def validate(self) -> Tuple[bool, List[str]]:
        if not self.order_id or self.place_sec < 0:
            return False, ["[FAIL] 成交时间基线场景未成功下单"]

        if self.first_fill_sec < 0:
            return True, ["[PASS] 本次样本未触发被动单成交（无倒灌迹象）"]

        passed = self.first_fill_sec > self.place_sec
        if passed:
            return True, [f"[PASS] 首笔成交时间晚于下单时间: place={self.place_sec}, first_fill={self.first_fill_sec}"]
        return False, [f"[FAIL] 出现同秒/提前成交: place={self.place_sec}, first_fill={self.first_fill_sec}"]
