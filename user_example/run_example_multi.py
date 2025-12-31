"""
旺财回测平台 - 多股票顺序回测示例

使用方法:
    python run_example_multi.py

说明:
    - 支持顺序测试多只股票
    - 在 SYMBOLS 列表中添加 (股票代码, 日期) 元组
    - 会依次运行每只股票的回测，最后输出总结

需要准备的数据文件:
    - cstick_{symbol}_{date}.csv  (Tick快照)
    - csord_{symbol}_{date}.csv   (逐笔委托)  
    - cstra_{symbol}_{date}.csv   (逐笔成交)
    - csbar1d_{symbol}_{date}.csv (日线数据，含涨跌停)
"""

import pandas as pd
from pathlib import Path

from wangcai_syn import run_backtest
from my_strategy import MyStrategy


# ========== 配置区 ==========
# 要测试的股票列表（股票代码, 日期）
SYMBOLS = [
    ("002929.SZ", "2025-12-15"),
    ("600105.SH", "2025-12-15"),
]
DATA_DIR = "../new_log"       # 数据目录
OUTPUT_DIR = "./output"       # 输出目录
# ============================


def load_data(symbol: str, date: str, data_dir: str):
    """加载回测所需的4个数据文件"""
    path = Path(data_dir)
    
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    
    print(f"✅ 数据加载完成: {symbol}")
    print(f"   Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    
    return cstick, csord, cstra, csbar1d


def main():
    results = []
    
    # 循环测试每只股票
    for idx, (symbol, date) in enumerate(SYMBOLS, 1):
        print(f"\n{'='*60}")
        print(f"📊 [{idx}/{len(SYMBOLS)}] 测试 {symbol} @ {date}")
        print(f"{'='*60}")
        
        try:
            # 1. 加载数据
            print(f"📖 加载数据...")
            cstick, csord, cstra, csbar1d = load_data(symbol, date, DATA_DIR)
            
            # 2. 组织数据格式
            data = {
                symbol: (cstick, csord, cstra, csbar1d)
            }
            
            # 3. 创建策略
            strategy = MyStrategy(account="user1")
            
            # 4. 运行回测
            print(f"🚀 开始回测...")
            success = run_backtest(
                data_dict=data,
                strategy=strategy,
                output_dir=f"{OUTPUT_DIR}/{symbol}_{date}"
            )
            
            # 5. 输出结果
            if success:
                print(f"✅ {symbol} 回测成功!")
                strategy.print_summary()
                results.append((symbol, date, "成功"))
            else:
                print(f"❌ {symbol} 回测失败")
                results.append((symbol, date, "失败"))
                
        except Exception as e:
            print(f"❌ {symbol} 发生错误: {e}")
            results.append((symbol, date, f"错误: {e}"))
            continue
    
    # 输出总结
    print(f"\n{'='*60}")
    print(f"📋 回测总结")
    print(f"{'='*60}")
    for symbol, date, status in results:
        status_icon = "✅" if status == "成功" else "❌"
        print(f"{status_icon} {symbol} @ {date}: {status}")
    print(f"{'='*60}")
    
    return 0


if __name__ == "__main__":
    exit(main())

