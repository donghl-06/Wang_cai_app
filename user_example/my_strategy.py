"""
旺财回测平台 - 策略模板

继承 Strategy 类，实现以下方法:
    - getStrategyId()    : 返回账户ID
    - onTickEvent()      : 处理Tick快照
    - onOrderEvent()     : 处理逐笔委托
    - onTradeEvent()     : 处理逐笔成交
    - onOrderCallback()  : 下单确认回调
    - onTradeCallback()  : 成交/撤单回调
"""

from typing import List
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)


class MyStrategy(Strategy):
    """
    策略：在 9:36:00 以卖1价买140单，只挂一次
    """

    def __init__(self, account: str = "user1"):
        super().__init__()
        self.account = account

        # 订单ID计数器
        self._order_counter = 1000

        # 策略状态
        self._order_placed = False
        self._pending_order = None  # (order_id, minute, price, volume)

        # 统计数据
        self.position = 0
        self.order_count = 0
        self.trade_count = 0
        self.cancel_count = 0

        # 记录未成交时的价格变化（用于调试）
        self._price_history = []

    def getStrategyId(self) -> str:
        """返回账户ID（必须实现）"""
        return self.account

    def _next_order_id(self) -> str:
        """生成下一个订单ID"""
        self._order_counter += 1
        return f"{self.account}_{self._order_counter}"

    # ==================== 事件处理 ====================

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        """
        策略场景：9:36:00 以卖1价买140单，只挂一次
        """
        events = []

        # 跳过集合竞价 (9:30之前)
        if tick.Time < 93000000:
            return events

        # 解析时间
        t = tick.Time // 1000  # 去掉毫秒
        second = t % 100
        t = t // 100
        minute = t % 100
        hour = t // 100
        current_minute = hour * 60 + minute
        time_str = f"{hour:02d}:{minute:02d}:{second:02d}"

        # 获取当前盘口
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0

        # 只在9:36:00挂一次
        if not self._order_placed and hour == 9 and minute == 36 and second == 0:
            if ask1 > 0:
                order_id = self._next_order_id()
                target_price = ask1
                order_volume = 140

                events.append(make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=order_id,
                    OrderType=0,       # 限价单
                    Direction=1,       # 买入
                    Price=target_price,
                    Volume=order_volume
                ))

                print(f"\n{'='*60}")
                print(f"🎯 [{time_str}] 以卖1价下买单")
                print(f"   订单ID: {order_id}")
                print(f"   价格: {target_price/10000:.4f} (卖1价)")
                print(f"   数量: {order_volume}")
                print(f"   当前 bid1: {bid1/10000:.4f}")
                print(f"   当前 ask1: {ask1/10000:.4f}")
                print(f"{'='*60}\n")

                self._order_placed = True
                self._pending_order = (order_id, current_minute, target_price, order_volume)
            else:
                print(f"[{time_str}] 无法下单，卖1价无效 (ask1={ask1})")

        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        """处理逐笔委托事件（可选实现）"""
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        """处理逐笔成交事件（可选实现）"""
        return []

    # ==================== 回调处理 ====================

    def onOrderCallback(self, cb: OrderCallback) -> None:
        """
        下单确认回调

        Args:
            cb: 回调数据，包含:
                - orderlocalid: 订单ID
                - direction: 1=买, 2=卖
                - price: 价格（厘）
                - volume: 数量
        """
        self.order_count += 1
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        """
        成交/撤单回调
        """
        if cb.matchtype == 'T':
            # 成交
            self.trade_count += 1
            self.position = cb.deltapos

            # 如果是pending的订单成交了，清除标记
            if self._pending_order and cb.localid == self._pending_order[0]:
                self._pending_order = None

            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")

        elif cb.matchtype == 'D':
            # 撤单
            self.cancel_count += 1

            if self._pending_order and cb.localid == self._pending_order[0]:
                self._pending_order = None

            print(f"  ☑️  撤单确认: {cb.localid}")

    # ==================== 辅助方法 ====================

    def print_summary(self):
        """打印策略摘要"""
        print(f"\n{'='*50}")
        print(f"📊 策略摘要: {self.account}")
        print(f"{'='*50}")
        print(f"  下单次数: {self.order_count}")
        print(f"  成交次数: {self.trade_count}")
        print(f"  撤单次数: {self.cancel_count}")
        print(f"  最终持仓: {self.position}")
        print(f"{'='*50}")

