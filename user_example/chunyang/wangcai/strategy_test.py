"""
接口验证策略 - 验证下单、撤单、成交功能
"""

from typing import List
from wangcai_syn import Strategy, UserEvent, Snapshot, make_order, make_cancel


class TestStrategy(Strategy):
    """
    接口验证策略
    - 下5个买单（远离市价，不成交）
    - 下5个卖单（接近市价，尝试成交）
    - 撤销未成交的买单
    """
    
    def __init__(self, account: str = "test"):
        super().__init__()
        self.account = account
        self.tick_count = 0
        self.pending_orders = []  # 待撤单的订单
        self.phase = 'wait'       # wait -> order -> cancel -> done
        
    def getStrategyId(self) -> str:
        return self.account
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        events = []
        self.tick_count += 1
        
        # 跳过前100个tick（等待进入连续竞价）
        if self.tick_count < 100:
            return events
        
        # 阶段1: 下单
        if self.phase == 'wait' and snapshot.bids[0] > 0:
            self.phase = 'order'
            print(f"\n{'='*40}")
            print(f"📝 开始下单测试")
            print(f"{'='*40}")
            
            # 下5个买单（低于买一价，不成交，用于测试撤单）
            for i in range(5):
                order_id = f"BUY_{i}"
                price = snapshot.bids[0] - 1000  # 低于买一价0.1元
                event = make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=snapshot.Exchange,
                    Instrument=snapshot.Instrument,
                    OrderLocalID=order_id,
                    OrderType=0,
                    Direction=1,
                    Price=price,
                    Volume=100
                )
                events.append(event)
                self.pending_orders.append(order_id)
                print(f"  下买单: {order_id} @ {price/10000:.4f} (待撤单)")
            
            # 下5个卖单（等于或高于卖一价，尝试成交）
            for i in range(5):
                order_id = f"SELL_{i}"
                price = snapshot.asks[0]  # 卖一价
                event = make_order(
                    Broker='',
                    Account=self.account,
                    Exchange=snapshot.Exchange,
                    Instrument=snapshot.Instrument,
                    OrderLocalID=order_id,
                    OrderType=0,
                    Direction=2,
                    Price=price,
                    Volume=100
                )
                events.append(event)
                print(f"  下卖单: {order_id} @ {price/10000:.4f} (尝试成交)")
        
        # 阶段2: 撤单（下一个tick执行）
        elif self.phase == 'order':
            self.phase = 'cancel'
            print(f"\n{'='*40}")
            print(f"❌ 开始撤单测试")
            print(f"{'='*40}")
            
            for order_id in self.pending_orders:
                event = make_cancel(
                    Broker='',
                    Account=self.account,
                    Exchange=snapshot.Exchange,
                    Instrument=snapshot.Instrument,
                    CancelOrderLocalID=f"C_{order_id}",
                    OrderLocalID=order_id
                )
                events.append(event)
                print(f"  撤单: {order_id}")
            
            self.phase = 'done'
        
        return events
    
    def onOrderCallback(self, callback):
        direction = "买" if callback.direction == 1 else "卖"
        print(f"📋 [下单确认] {callback.orderlocalid} {direction} "
              f"{callback.volume}@{callback.price/10000:.4f}")
    
    def onTradeCallback(self, callback):
        if callback.matchtype == 'T':
            direction = "买" if callback.direction == 'B' else "卖"
            print(f"✅ [成交] {callback.localid} {direction} "
                  f"{callback.volume}@{callback.price/10000:.4f} "
                  f"持仓:{callback.deltapos}")
        elif callback.matchtype == 'D':
            print(f"✅ [撤单确认] {callback.localid}")
    
    def print_summary(self):
        print(f"\n{'='*40}")
        print(f"📊 测试完成")
        print(f"{'='*40}")

