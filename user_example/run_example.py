"""
旺财回测平台 - 运行示例

使用方法:
    python run_example.py

需要准备的数据文件（放在 data/ 目录下）:
    - cstick_{symbol}_{date}.csv  (Tick快照)
    - csord_{symbol}_{date}.csv   (逐笔委托)  
    - cstra_{symbol}_{date}.csv   (逐笔成交)
    - csbar1d_{symbol}_{date}.csv (日线数据，含涨跌停)
"""

import sys
import os


import pandas as pd
from pathlib import Path
import wangcai_syn
from my_strategy import MyStrategy


# ========== 配置区 ==========
SYMBOL = "300827.SZ"          # 股票代码
DATE = "2025-11-17"           # 回测日期
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
    # 1. 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    # 2. 组织数据格式
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }
    
    # 3. 创建策略
    strategy = MyStrategy(account="user1")
    
    # 4. 运行回测
    print(f"\n🚀 开始回测...")
    success = wangcai_syn.run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}"
    )
    
    # 5. 输出结果
    if success:
        print(f"\n🎉 回测完成!")
        strategy.print_summary()
    else:
        print(f"\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())

