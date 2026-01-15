#!/usr/bin/env python3
"""
测试Python策略中耗时操作对回测结束时机的影响
"""

import sys
import time
import os
from typing import List

# 添加本地编译的模块路径
sys.path.insert(0, './build')

from wangcai_cpp import (
    BacktestEngine, Strategy,
    Direction, OrderType,
    Event, UserEvent, Execution, Snapshot,
    OrderDetail, TradeDetail,
    make_order_event, make_cancel_event,
    TradeCallback, OrderCallback, MultiBacktestEngine
)

class SleepTestStrategy(Strategy):
    """
    测试策略：模拟在事件处理中有耗时操作的场景
    现在完全自动检测，不需要手动管理状态
    """
    
    def __init__(self, strategy_id: str = "SLEEP_TEST"):
        super().__init__()
        self.strategy_id = strategy_id
        self.event_count = 0
        self.sleep_duration = 0.00001  # 每次处理睡眠0.1秒
        
        print(f"🔄 [{self.strategy_id}] 耗时测试策略已启动")
        print(f"配置：每个事件处理时间 {self.sleep_duration} 秒")
        print(f"✨ 自动状态检测：无需手动管理处理状态")
    
    def getStrategyId(self) -> str:
        return self.strategy_id
    
    def onOrderEvent(self, order) -> List[UserEvent]:
        """处理订单事件 - 带耗时操作（自动状态跟踪）"""
        self.event_count += 1
        
        print(f"🔄 [{self.strategy_id}] 开始处理订单事件 #{self.event_count} "
              f"(订单ID: {order.OrderNo}, 价格: {order.Price/10000:.4f})")
        
        # 模拟耗时计算 - C++层会自动跟踪这个处理过程
        time.sleep(self.sleep_duration)
        
        print(f"✅ [{self.strategy_id}] 完成处理订单事件 #{self.event_count}")
        
        return []
    
    def onTradeEvent(self, trade) -> List[UserEvent]:
        """处理成交事件 - 带耗时操作（自动状态跟踪）"""
        self.event_count += 1
        
        print(f"🔄 [{self.strategy_id}] 开始处理成交事件 #{self.event_count} "
              f"(价格: {trade.Price/10000:.4f}, 数量: {trade.Volume})")
        
        # 模拟耗时计算 - C++层会自动跟踪这个处理过程
        time.sleep(self.sleep_duration)
        
        print(f"✅ [{self.strategy_id}] 完成处理成交事件 #{self.event_count}")
        
        return []
    
    def onTickEvent(self, snapshot: Snapshot) -> List[UserEvent]:
        """处理Tick事件 - 带耗时操作（自动状态跟踪）"""
        self.event_count += 1
        
        # 只处理部分Tick事件，避免过多输出
        if self.event_count % 100 == 1:
            print(f"🔄 [{self.strategy_id}] 开始处理Tick事件 #{self.event_count} "
                  f"(时间: {snapshot.datetime}, 最新价: {snapshot.last_price/10000:.4f})")
        
        # 模拟耗时计算 - C++层会自动跟踪这个处理过程
        time.sleep(self.sleep_duration)
        
        if self.event_count % 100 == 1:
            print(f"✅ [{self.strategy_id}] 完成处理Tick事件 #{self.event_count}")
        
        return []
    
    def onTradeCallback(self, callback) -> None:
        """交易回调"""
        print(f"📈 [{self.strategy_id}] 交易回调: {callback.localid}")
    
    def onOrderCallback(self, callback) -> None:
        """下单回调"""
        print(f"📋 [{self.strategy_id}] 下单回调: {callback.orderlocalid}")
    
    def onOrderFilled(self, order_id: str, price: int, volume: int) -> None:
        """订单成交回调"""
        print(f"✅ [{self.strategy_id}] 订单成交: {order_id}")
    
    def onOrderCancelled(self, order_id: str, reason: str) -> None:
        """订单撤销回调"""
        print(f"❌ [{self.strategy_id}] 订单撤销: {order_id} - {reason}")
    
    def print_status(self):
        """打印状态"""
        print(f"📊 [{self.strategy_id}] 状态:")
        print(f"   - 总处理事件数: {self.event_count}")
        print(f"   - 状态检测: 自动（C++层管理）")
        print(f"   - 完成状态: 由isProcessingComplete()自动检测")


def test_sleep_strategy(symbol: str, date: str, data_path: str):
    """测试耗时策略的回测结束时机"""
    print("🧪 开始测试耗时策略对回测结束时机的影响")
    print(f"合约: {symbol}")
    print(f"日期: {date}")
    print(f"数据路径: {data_path}")
    
    try:
        # 创建回测引擎
        engine = MultiBacktestEngine([symbol], date, data_path)
        
        # 创建耗时测试策略
        strategy = SleepTestStrategy("SLEEP_TEST")
        engine.registerStrategy(strategy)
        
        print(f"\n🚀 开始运行回测...")
        start_time = time.time()
        
        # 运行回测
        engine.run()
        
        end_time = time.time()
        
        print(f"\n⏱️  回测运行完成，总耗时: {end_time - start_time:.2f} 秒")
        
        # 检查策略状态
        strategy.print_status()
        
        # 获取结果
        positions = engine.getPositions()
        total_pnl = engine.getTotalPnL()
        
        print(f"\n💰 回测结果:")
        print(f"   - 最终盈亏: {total_pnl:.2f} 元")
        
        # 检查策略完成状态（现在是自动检测的）
        is_complete = strategy.isProcessingComplete()
        if not is_complete:
            print(f"❌ 发现问题：策略报告未完成处理！")
            print("   这说明回测在策略处理完成前就结束了")
            return False
        else:
            print(f"✅ 所有事件都已处理完成（自动检测）")
            return True
        
    except Exception as e:
        print(f"❌ 测试失败: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """主函数"""
    # 配置参数
    symbol = "000488.SZ"
    date = "2024-12-19"
    data_path = "logs"
    
    # 检查数据文件
    required_files = [
        f"cstick_{symbol}_{date}.csv",
        f"cstra_{symbol}_{date}.csv", 
        f"csord_{symbol}_{date}.csv"
    ]
    
    print(f"检查数据文件...")
    missing_files = []
    for file in required_files:
        file_path = os.path.join(data_path, file)
        if not os.path.exists(file_path):
            missing_files.append(file)
        else:
            print(f"✅ 找到文件: {file}")
    
    if missing_files:
        print("❌ 缺少以下数据文件:")
        for file in missing_files:
            print(f"   {os.path.join(data_path, file)}")
        return 1
    
    print("✅ 数据文件检查通过")
    
    # 运行测试
    success = test_sleep_strategy(symbol, date, data_path)
    
    if success:
        print(f"\n🎉 测试通过：回测引擎正确等待了策略处理完成！")
    else:
        print(f"\n❌ 测试失败：发现了同步问题！")
    
    # 在程序结束前睡眠10秒（总共处理了35793个订单）
    print(f"\n⏰ 程序即将结束，睡眠10秒...")
    time.sleep(10)
    print("💤 睡眠完成，程序结束")
    
    return 0 if success else 1


if __name__ == "__main__":
    exit(main())
