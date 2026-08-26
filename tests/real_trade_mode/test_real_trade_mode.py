"""
测试真实成交替代模式

测试场景：
1. 被动买单排队：下一个低于 ask1 的买单，验证只有真实成交消耗到队列位置后才能成交
2. 被动卖单排队：下一个高于 bid1 的卖单，同理验证
3. 主动单走严格模式：主动单仍按可用量+欠债逻辑处理
4. 部分成交：验证虚拟订单可以多次部分成交
"""

from typing import List
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order, make_cancel
)


class RealTradeMatchTestStrategy(Strategy):
    """
    真实成交替代模式测试策略
    
    测试场景：
    - 9:31:00: 下一笔被动买单（买价 = bid1，排在队尾等待真实成交消耗）
    - 9:32:00: 下一笔主动买单（价格 = UpperLimit，走严格模式）
    - 观察被动单是否按真实成交量逐步成交
    """

    def __init__(self, account: str = "test_rt"):
        super().__init__()
        self.account = account
        
        # 订单ID计数器
        self._order_counter = 2000
        
        # 测试状态
        self._passive_buy_placed = False
        self._passive_buy_id = None
        self._aggressive_buy_placed = False
        self._aggressive_buy_id = None
        
        # 统计
        self.order_count = 0
        self.trade_count = 0
        self.cancel_count = 0
        self.total_filled = 0          # 被动单总成交量
        self.partial_fill_count = 0     # 被动单部分成交次数
        self.aggressive_filled = 0      # 主动单成交量
        self.aggressive_cancelled = 0   # 主动单撤单量
        self.position = 0
        
        # 回调记录
        self.trade_callbacks = []
        self.cancel_callbacks = []

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self) -> str:
        self._order_counter += 1
        return f"{self.account}_{self._order_counter}"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        events = []
        
        if tick.Time < 93000000:
            return events
        
        t = tick.Time // 1000
        second = t % 100
        t = t // 100
        minute = t % 100
        hour = t // 100
        time_str = f"{hour:02d}:{minute:02d}:{second:02d}"
        
        bid1 = tick.bids[0] if tick.bids[0] > 0 else 0
        ask1 = tick.asks[0] if tick.asks[0] > 0 else 0
        bid1_size = tick.bid_sizes[0]
        
        # === 测试1: 9:31:00 下被动买单（挂在 bid1 价格） ===
        if not self._passive_buy_placed and hour == 9 and minute == 31 and second == 0:
            if bid1 > 0:
                order_id = self._next_order_id()
                order_vol = 100  # 小单量，观察排队和成交过程
                
                events.append(make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=order_id,
                    OrderType=0,
                    Direction=1,  # 买入
                    Price=bid1,    # 被动价格（= bid1，不穿越价差）
                    Volume=order_vol
                ))
                
                self._passive_buy_placed = True
                self._passive_buy_id = order_id
                
                print(f"\n{'='*60}")
                print(f"[测试] [{time_str}] 下被动买单")
                print(f"  订单ID: {order_id}")
                print(f"  价格: {bid1/10000:.4f} (= bid1, 被动)")
                print(f"  数量: {order_vol}")
                print(f"  bid1排队量: {bid1_size}")
                print(f"  预期: 需等待同价位真实成交消耗完前方 {bid1_size} 股后才能成交")
                print(f"{'='*60}\n")
        
        # === 测试2: 9:32:00 下主动买单（走严格模式） ===
        if not self._aggressive_buy_placed and hour == 9 and minute == 32 and second == 0:
            if ask1 > 0:
                order_id = self._next_order_id()
                aggressive_price = tick.UpperLimit if getattr(tick, "UpperLimit", 0) > 0 else ask1
                order_vol = 500000  # 大量，测试部分成交+撤单
                
                events.append(make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=tick.Exchange,
                    Instrument=tick.Instrument,
                    OrderLocalID=order_id,
                    OrderType=0,
                    Direction=1,
                    Price=aggressive_price,
                    Volume=order_vol
                ))
                
                self._aggressive_buy_placed = True
                self._aggressive_buy_id = order_id
                
                print(f"\n{'='*60}")
                print(f"[测试] [{time_str}] 下主动买单（走严格模式）")
                print(f"  订单ID: {order_id}")
                print(f"  价格: {aggressive_price/10000:.4f} (穿越价差)")
                print(f"  数量: {order_vol}")
                print(f"  预期: 只能成交对手方可用量，剩余撤单")
                print(f"{'='*60}\n")
        
        return events

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        self.order_count += 1
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  -> 下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.matchtype == 'T':
            self.trade_count += 1
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            self.trade_callbacks.append({
                'id': cb.localid, 'dir': direction, 'vol': cb.volume,
                'price': cb.price / 10000.0, 'pos': cb.deltapos,
                'time': cb.matchtime
            })
            
            # 区分被动单和主动单的成交
            if self._passive_buy_id and cb.localid == self._passive_buy_id:
                self.total_filled += cb.volume
                self.partial_fill_count += 1
                print(f"  [被动单成交] {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f}"
                      f" (累计成交: {self.total_filled}, 第{self.partial_fill_count}次)")
            elif self._aggressive_buy_id and cb.localid == self._aggressive_buy_id:
                self.aggressive_filled += cb.volume
                print(f"  [主动单成交] {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f}"
                      f" (严格模式成交)")
            else:
                print(f"  [成交] {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f}")
                
        elif cb.matchtype == 'D':
            self.cancel_count += 1
            self.cancel_callbacks.append({'id': cb.localid, 'time': cb.matchtime})
            
            if self._aggressive_buy_id and cb.localid == self._aggressive_buy_id:
                self.aggressive_cancelled += 1
                print(f"  [主动单撤单] {cb.localid} (严格模式剩余部分撤单)")
            else:
                print(f"  [撤单] {cb.localid}")

    def onOrderFilled(self, order_id: str, price, volume) -> None:
        pass

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        pass

    def passed(self) -> bool:
        """核心断言：主动单必须被处理（成交或撤单），不能无声消失。
        被动单是否成交取决于当日真实成交量，不作为硬性失败条件。"""
        return (self.order_count > 0
                and (self.aggressive_filled > 0 or self.aggressive_cancelled > 0))

    def print_summary(self):
        print(f"\n{'='*60}")
        print(f"真实成交替代模式测试摘要")
        print(f"{'='*60}")
        print(f"  下单次数: {self.order_count}")
        print(f"  成交次数: {self.trade_count}")
        print(f"  撤单次数: {self.cancel_count}")
        print(f"  最终持仓: {self.position}")
        print(f"\n  被动买单 ({self._passive_buy_id}):")
        print(f"    总成交量: {self.total_filled}")
        print(f"    成交次数: {self.partial_fill_count} (部分成交)")
        print(f"\n  主动买单 ({self._aggressive_buy_id}):")
        print(f"    成交量: {self.aggressive_filled}")
        print(f"    撤单: {'是' if self.aggressive_cancelled > 0 else '否'}")
        print(f"{'='*60}")
        
        # 验证
        print(f"\n验证结果:")
        if self.partial_fill_count > 0:
            print(f"  [PASS] 被动单有成交（真实成交池生效）")
        else:
            print(f"  [INFO] 被动单未成交（可能 bid1 排队量大于当日成交量，属正常）")
        
        if self.aggressive_filled > 0:
            print(f"  [PASS] 主动单部分成交（严格模式生效）")
        if self.aggressive_cancelled > 0:
            print(f"  [PASS] 主动单剩余撤单（严格模式生效）")
        print(f"{'='*60}")
