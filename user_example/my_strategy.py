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
    示例策略：每分钟开头下一个买一价限价单，下一分钟没成交就撤单
    
    用户可以修改 onTickEvent 中的逻辑来实现自己的策略
    """
    
    def __init__(self, account: str = "user1"):
        super().__init__()
        self.account = account
        
        # 订单ID计数器
        self._order_counter = 1000
        
        # 策略状态
        self._last_minute = -1
        self._pending_order = None  # (order_id, minute)
        
        # 统计数据
        self.position = 0
        self.order_count = 0
        self.trade_count = 0
        self.cancel_count = 0
    
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
        处理Tick快照事件
        
        Args:
            tick: Tick数据，包含:
                - Time: 时间戳 (HHMMSSmmm格式, 如 93000000)
                - bids[0-9]: 买一到买十价格
                - asks[0-9]: 卖一到卖十价格
                - bid_sizes[0-9]: 买一到买十数量
                - ask_sizes[0-9]: 卖一到卖十数量
        
        Returns:
            要执行的事件列表 (下单/撤单)
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
        
        # === 撤单逻辑：上一分钟的订单还在，撤掉 ===
        if self._pending_order and current_minute > self._pending_order[1]:
            order_id = self._pending_order[0]
            events.append(make_cancel(
                Broker='',
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                CancelOrderLocalID=f"C_{order_id}",
                OrderLocalID=order_id
            ))
            print(f"⏰ [{hour:02d}:{minute:02d}:{second:02d}] 撤单: {order_id}")
            self._pending_order = None
        
        # === 下单逻辑：分钟开头下买一价限价单 ===
        if second <= 1 and current_minute != self._last_minute and tick.bids[0] > 0:
            order_id = self._next_order_id()
            price = tick.bids[0]  # 买一价
            
            events.append(make_order(
                Broker='',
                Account=self.account,
                Exchange=tick.Exchange,
                Instrument=tick.Instrument,
                OrderLocalID=order_id,
                OrderType=0,       # 限价单
                Direction=1,       # 买入
                Price=price,
                Volume=100
            ))
            
            print(f"📝 [{hour:02d}:{minute:02d}:{second:02d}] 下单: {order_id} @ {price/10000:.4f}")
            
            self._last_minute = current_minute
            self._pending_order = (order_id, current_minute)
        
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
        
        Args:
            cb: 回调数据，包含:
                - localid: 订单ID
                - matchtype: 'T'=成交, 'D'=撤单
                - direction: 'B'=买, 'S'=卖
                - volume: 数量
                - price: 价格（厘）
                - deltapos: 当前持仓
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
            
            print(f"  ☑️  撤单成功: {cb.localid}")
    
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

