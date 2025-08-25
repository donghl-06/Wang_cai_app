'''
Author: chenlisen
Date: 2025-08-18 12:40:34
LastEditTime: 2025-08-25 01:48:25
FilePath: /wangcai_cpp/run.py
'''
import os
from datetime import datetime
from typing import Dict, Any, List

import wangcai_bt as wc

class SimpleStrategy(wc.Strategy):
    """简单的均价回归策略示例"""
    def __init__(self, strategy_id: str):
        super().__init__()
        self.strategy_id = strategy_id
        self.order_count = 0
        self.position = 0
        self.last_price = 0
        self.ma_prices = []  # 存储最近的价格用于计算均价
        self.ma_period = 20  # 均价周期
        self.id_prefix = int(strategy_id.split("_")[-1]) * 1000000
    
    def generate_order_id(self):
        self.order_count += 1
        return f"{self.strategy_id}_{self.order_count}"
    
    def getStrategyId(self) -> str:
        return self.strategy_id

    def onOrderEvent(self, event) -> List:
        """处理逐笔委托"""
        return []

    def onTradeEvent(self, execution, datetime: str) -> List:
        """处理逐笔成交"""
        print(f"[{self.strategy_id}] 成交事件: "
            f"价格={execution.price/10000:.2f}, "
            f"数量={execution.volume}, "
            f"时间={datetime}")
        return []

    def onTickEvent(self, snapshot) -> List:
        """处理行情快照"""
        user_events = []
        
        # 更新最新价格
        if snapshot.last_price > 0:
            self.last_price = snapshot.last_price
            
            # 维护价格列表
            self.ma_prices.append(self.last_price)
            if len(self.ma_prices) > self.ma_period:
                self.ma_prices.pop(0)
            
            # 计算均价
            if len(self.ma_prices) >= self.ma_period:
                ma_price = sum(self.ma_prices) / len(self.ma_prices)
              
                if self.last_price < ma_price * 0.98 and self.position <= 0:
                    # 价格低于均价2%，买入
                    self.order_count += 1
                    order_id = self.generate_order_id()
                    
                    # 创建买单
                    order_event = wc.make_order_event(
                        order_id=order_id,
                        symbol=snapshot.Instrument,
                        direction=wc.Direction.Buy,
                        order_type=wc.OrderType.Limit,
                        price=snapshot.asks[0] if len(snapshot.asks) > 0 else self.last_price,
                        volume=100,
                        strategy_id=self.strategy_id
                    )
                    user_events.append(order_event)
                    self.position += 100
                    print(f"[{self.strategy_id}] 发出买单: {order_id}")
                    
                elif self.last_price > ma_price * 1.02 and self.position > 0:
                    # 价格高于均价2%，卖出
                    self.order_count += 1
                    order_id = self.generate_order_id()
                    
                    # 创建卖单
                    order_event = wc.make_order_event(
                        order_id=order_id,
                        symbol=snapshot.Instrument,
                        direction=wc.Direction.Sell,
                        order_type=wc.OrderType.Limit,
                        price=snapshot.bids[0] if len(snapshot.bids) > 0 else self.last_price,
                        volume=100,
                        strategy_id=self.strategy_id
                    )
                    user_events.append(order_event)
                    self.position -= 100
                    print(f"[{self.strategy_id}] 发出卖单: {order_id}")
        
        return user_events


    def onOrderCancelled(self, order_id: str, reason: str):
        """订单撤销通知"""
        print(f"[{self.strategy_id}] 订单撤销: {order_id}, 原因={reason}")

    def onTradeCallback(self, callback):
        """交易回调"""
        print(f"[{self.strategy_id}] 交易回调: "
            f"订单={callback.localid}, "
            f"方向={callback.direction}, "
            f"数量={callback.volume}, "
            f"价格={callback.price/10000:.2f}, "
            f"持仓={callback.deltapos}")

    def onOrderCallback(self, callback):
        """订单回调"""
        print(f"[{self.strategy_id}] 订单回调: "
            f"订单={callback.orderlocalid}, "
            f"方向={callback.direction}, "
            f"数量={callback.volume}, "
            f"价格={callback.price/10000:.2f}")

            
if __name__ == "__main__":
    # 设置回测参数
    symbols = ["600227.SH", "600281.SH"] 
    date = "2023-12-22" 
    data_path = "logs"

    # 创建回测引擎
    engine = wc.MultiBacktestEngine(symbols, date, data_path)

    # 创建并注册多个策略
    strategy1 = SimpleStrategy("STRATEGY_001")
    # strategy2 = SimpleStrategy("STRATEGY_002")

    engine.registerStrategy(strategy1)
    # engine.registerStrategy(strategy2)

    # 运行回测
    print("\n开始运行回测...")
    engine.run()

    # 获取回测结果
    positions = engine.getPositions()
    total_pnl = engine.getTotalPnL()

    print(f"\n回测完成!")
    print(f"总未实现盈亏: {total_pnl:.2f}")
    print("\n各策略持仓详情:")
    for key, pos in positions.items():
        if pos.quantity != 0:
            print(f"  {key}: 数量={pos.quantity}, "
                f"成本={pos.avg_cost:.4f}, "
                f"已实现盈亏={pos.realized_pnl:.2f}")