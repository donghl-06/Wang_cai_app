"""
简单策略示例

这个示例展示了如何使用 wangcai_syn 编写一个简单的交易策略
"""

import os
from wangcai_syn import (
    MultiBacktestEngine,
    Strategy,
    Direction,
    OrderType,
    make_order_event,
    make_cancel_event,
)


class SimpleStrategy(Strategy):
    """
    简单策略示例：
    - 在 Tick 事件上，如果价格合适就下单
    - 记录所有回调
    """
    
    def __init__(self, strategy_id="SIMPLE"):
        super().__init__()
        self.strategy_id = strategy_id
        self.order_counter = 0
        self.tick_count = 0
        self.trade_count = 0
        
        print(f"[{self.strategy_id}] 策略已启动")
    
    def getStrategyId(self):
        return self.strategy_id
    
    def onTickEvent(self, snapshot):
        """处理 Tick 事件"""
        events = []
        
        # 每 100 个 Tick 处理一次
        self.tick_count += 1
        if self.tick_count % 100 != 0:
            return events
        
        # 只在连续竞价时段交易
        time_value = snapshot.Time
        hour = time_value // 10000000
        minute = (time_value // 100000) % 100
        
        # 09:30-11:30, 13:00-14:55
        if not ((hour == 9 and minute >= 30) or 
                (hour == 10) or 
                (hour == 11 and minute <= 30) or
                (hour == 13) or
                (hour == 14 and minute <= 55)):
            return events
        
        # 获取最新价格
        last_price = snapshot.last_price
        if last_price <= 0:
            return events
        
        # 简单策略：每次买入 100 股
        if self.order_counter < 5:  # 限制下单次数
            order_id = f"{self.strategy_id}_{self.order_counter:04d}"
            self.order_counter += 1
            
            # 以略高于市价的价格买入
            buy_price = last_price + 100  # 加 0.01 元
            
            event = make_order_event(
                order_id=order_id,
                symbol=snapshot.Instrument,
                direction=Direction.Buy,
                order_type=OrderType.Limit,
                price=buy_price,
                volume=100,
                strategy_id=self.strategy_id
            )
            events.append(event)
            
            print(f"📝 下单: {order_id}, 价格: {buy_price/10000:.2f} 元, 数量: 100")
        
        return events
    
    def onOrderEvent(self, order):
        """处理订单事件"""
        return []
    
    def onTradeEvent(self, trade):
        """处理成交事件"""
        return []
    
    def onOrderCallback(self, callback):
        """下单回调"""
        print(f"✅ 下单确认: {callback.orderlocalid}, "
              f"价格: {callback.price/10000:.2f} 元")
    
    def onTradeCallback(self, callback):
        """交易回调"""
        if callback.matchtype == 'T':  # 成交
            self.trade_count += 1
            print(f"💰 成交 #{self.trade_count}: {callback.localid}, "
                  f"价格: {callback.price/10000:.2f} 元, "
                  f"数量: {callback.volume}, "
                  f"持仓: {callback.deltapos}")


def main():
    """运行简单策略示例"""
    # 配置参数
    symbol = "000488.SZ"
    date = "2024-12-19"
    data_path = "./logs"  # 数据文件路径
    
    print("=" * 60)
    print("🚀 旺财回测引擎 - 简单策略示例")
    print("=" * 60)
    print(f"标的: {symbol}")
    print(f"日期: {date}")
    print(f"数据路径: {data_path}")
    print()
    
    # 检查数据文件
    required_files = [
        f"cstick_{symbol}_{date}.csv",
        f"cstra_{symbol}_{date}.csv",
        f"csord_{symbol}_{date}.csv",
    ]
    
    print("检查数据文件...")
    missing_files = []
    for file in required_files:
        file_path = os.path.join(data_path, file)
        if not os.path.exists(file_path):
            missing_files.append(file)
            print(f"❌ 缺少: {file}")
        else:
            print(f"✅ 找到: {file}")
    
    if missing_files:
        print("\n❌ 缺少数据文件，无法运行回测")
        return 1
    
    print("\n" + "=" * 60)
    print("开始回测...")
    print("=" * 60 + "\n")
    
    try:
        # 创建回测引擎
        engine = MultiBacktestEngine(
            symbols=[symbol],
            date=date,
            data_path=data_path
        )
        
        # 创建并注册策略
        strategy = SimpleStrategy("SIMPLE")
        engine.registerStrategy(strategy)
        
        # 运行回测
        engine.run()
        
        # 获取结果
        positions = engine.getPositions()
        total_pnl = engine.getTotalPnL()
        
        # 打印结果
        print("\n" + "=" * 60)
        print("📊 回测结果")
        print("=" * 60)
        print(f"最终持仓: {positions}")
        print(f"总盈亏: {total_pnl:.2f} 元")
        print(f"下单次数: {strategy.order_counter}")
        print(f"成交次数: {strategy.trade_count}")
        print("=" * 60)
        
        print("\n✅ 回测完成！")
        return 0
        
    except Exception as e:
        print(f"\n❌ 回测失败: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    exit(main())

