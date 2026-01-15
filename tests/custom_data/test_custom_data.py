"""
用户自定义数据推送功能测试

测试内容：
1. 自定义数据按时间推送给策略
2. 策略可以根据自定义数据下单
"""

from typing import List, Dict
from wangcai_syn import (
    Strategy,
    UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
    make_order
)


class CustomDataTestStrategy(Strategy):
    """
    自定义数据推送测试策略
    
    收到自定义数据后，根据 signal 字段决定是否下单
    """

    def __init__(self, account: str = "custom_test"):
        super().__init__()
        self.account = account
        
        # 订单计数
        self._order_counter = 5000
        
        # 记录收到的自定义数据
        self.received_custom_events = []
        
        # 记录下单情况
        self.orders_placed = []
        
        # 持仓
        self.position = 0
        
        # 缓存最新的 tick 数据（用于获取合约代码等）
        self._last_tick = None

    def getStrategyId(self) -> str:
        return self.account

    def _next_order_id(self) -> str:
        self._order_counter += 1
        return f"{self.account}_{self._order_counter}"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        """缓存最新的 tick 数据"""
        self._last_tick = tick
        return []

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onCustomEvent(self, data: Dict) -> List[UserEvent]:
        """
        接收自定义数据回调
        
        Args:
            data: 自定义数据字典，包含 datetime 和其他用户定义的字段
        
        Returns:
            下单事件列表
        """
        events = []
        
        # 记录收到的数据
        self.received_custom_events.append(data)
        
        datetime_str = data.get('datetime', 'unknown')
        signal_raw = data.get('signal', 0)
        target_price = data.get('target_price', 0)
        
        print(f"\n📌 [自定义数据] 收到数据:")
        print(f"   时间: {datetime_str}")
        print(f"   信号: {signal_raw}")
        print(f"   目标价格: {target_price}")
        
        # 根据信号下单
        signal = 0
        if isinstance(signal_raw, str):
            signal_str = signal_raw.strip().lower()
            if signal_str in ("yes", "y", "true", "1", "buy", "b"):
                signal = 1
            elif signal_str in ("sell", "s"):
                signal = -1
            else:
                signal = 0
        else:
            try:
                signal = int(signal_raw)
            except Exception:
                signal = 0
        
        if signal != 0 and self._last_tick is not None:
            order_id = self._next_order_id()
            direction = 1 if signal > 0 else 2  # 1=买入, 2=卖出
            
            # 使用目标价格或当前价格
            if target_price > 0:
                price = int(target_price * 10000)  # 转换为厘
            else:
                price = self._last_tick.asks[0] if signal > 0 else self._last_tick.bids[0]
            
            events.append(make_order(
                Broker='',
                Account=self.account,
                Exchange=self._last_tick.Exchange,
                Instrument=self._last_tick.Instrument,
                OrderLocalID=order_id,
                OrderType=0,
                Direction=direction,
                Price=price,
                Volume=100
            ))
            
            direction_str = "买入" if signal > 0 else "卖出"
            print(f"   ✅ 根据信号下单: {order_id} {direction_str} 100@{price/10000:.4f}")
            
            self.orders_placed.append({
                'order_id': order_id,
                'direction': direction_str,
                'price': price,
                'volume': 100,
                'signal': signal
            })
        
        return events

    def onOrderCallback(self, cb: OrderCallback) -> None:
        direction = "买" if cb.direction == 1 else "卖"
        print(f"  ✓ 下单确认: {cb.orderlocalid} {direction} {cb.volume}@{cb.price/10000:.4f}")

    def onTradeCallback(self, cb: TradeCallback) -> None:
        if cb.matchtype == 'T':
            self.position = cb.deltapos
            direction = "买" if cb.direction == 'B' else "卖"
            print(f"  ✅ 成交: {cb.localid} {direction} {cb.volume}@{cb.price/10000:.4f} 持仓={self.position}")
        elif cb.matchtype == 'D':
            print(f"  ☑️  撤单: {cb.localid}")

    def onOrderFilled(self, order_id: str, price, volume) -> None:
        pass

    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        pass

    def print_summary(self):
        """打印测试结果"""
        print(f"\n{'='*70}")
        print(f"📊 自定义数据推送测试结果")
        print(f"{'='*70}")
        print(f"  收到自定义数据次数: {len(self.received_custom_events)}")
        print(f"  下单次数: {len(self.orders_placed)}")
        print(f"  最终持仓: {self.position}")
        
        if self.received_custom_events:
            print(f"\n  收到的数据:")
            for i, data in enumerate(self.received_custom_events):
                print(f"    [{i+1}] {data}")
        
        if self.orders_placed:
            print(f"\n  下单记录:")
            for i, order in enumerate(self.orders_placed):
                print(f"    [{i+1}] {order['order_id']} {order['direction']} {order['volume']}@{order['price']/10000:.4f} (signal={order['signal']})")
        
        print(f"{'='*70}")
        
        # 检查测试是否通过
        if len(self.received_custom_events) > 0:
            print(f"🎉 测试通过！自定义数据推送功能正常工作")
        else:
            print(f"⚠️  未收到自定义数据")
        
        print(f"{'='*70}")
