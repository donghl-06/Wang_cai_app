"""
接口测试示例

这个示例展示了如何使用内置的 InterfaceTestStrategy 进行接口验证
"""

import os
from wangcai_syn import MultiBacktestEngine
from wangcai_syn.strategy_base import InterfaceTestStrategy


def main():
    """运行接口测试"""
    # 配置参数
    symbol = "000488.SZ"
    date = "2024-12-19"
    data_path = "./logs"
    output_dir = f"./interface_test_output/{symbol}_{date}"
    
    print("=" * 60)
    print("🧪 旺财回测引擎 - 接口测试")
    print("=" * 60)
    print(f"标的: {symbol}")
    print(f"日期: {date}")
    print(f"数据路径: {data_path}")
    print(f"输出路径: {output_dir}")
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
        print("\n❌ 缺少数据文件，无法运行测试")
        return 1
    
    print("\n" + "=" * 60)
    print("开始接口测试...")
    print("=" * 60 + "\n")
    
    try:
        # 创建回测引擎
        engine = MultiBacktestEngine(
            symbols=[symbol],
            date=date,
            data_path=data_path
        )
        
        # 创建接口测试策略
        strategy = InterfaceTestStrategy("INTERFACE_TEST")
        engine.registerStrategy(strategy)
        
        # 运行回测
        engine.run()
        
        # 获取结果
        positions = engine.getPositions()
        total_pnl = engine.getTotalPnL()
        
        print(f"\n✅ 接口测试完成!")
        print(f"最终盈亏: {total_pnl:.2f} 元")
        
        # 保存记录
        print(f"\n保存测试记录到: {output_dir}")
        strategy.save_records(output_dir)
        
        # 打印测试报告
        strategy.print_test_report()
        
        print(f"\n📁 输出的 7 个文件：")
        print(f"   【回调记录】")
        print(f"   1️⃣  order_callbacks.csv   - 下单回调记录")
        print(f"   2️⃣  cancel_callbacks.csv  - 撤单回调记录")
        print(f"   3️⃣  trade_callbacks.csv   - 成交回调记录")
        print(f"   4️⃣  position_updates.csv  - 持仓信息更新")
        print(f"   【原始市场数据】")
        print(f"   5️⃣  tick_events.csv       - Tick事件记录")
        print(f"   6️⃣  order_events.csv      - 订单事件记录")
        print(f"   7️⃣  trade_events.csv      - 成交事件记录")
        
        print("\n✅ 接口测试成功完成！")
        return 0
        
    except Exception as e:
        print(f"\n❌ 接口测试失败: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    exit(main())

