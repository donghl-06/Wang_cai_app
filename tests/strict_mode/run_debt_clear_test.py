#!/usr/bin/env python3
"""
运行欠债清零测试

验证：欠债清零后可以继续下主动单
"""

import os
import sys
from pathlib import Path

import pandas as pd

# 确保使用本地编译的包
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../..'))

import wangcai_syn
from wangcai_syn import run_backtest

print(f"📦 wangcai_syn: {wangcai_syn.__file__}")
print(f"📦 扩展模块: {wangcai_syn.wangcai_cpp.__file__}")

from test_debt_clear import DebtClearTestStrategy


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../../new_log"
OUTPUT_DIR = "../../user_example/output"
# ============================


def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"✅ 数据加载完成: {symbol}")
    print(f"   Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main():
    print(f"\n{'='*70}")
    print(f"🧪 欠债清零测试")
    print(f"{'='*70}")
    print(f"  标的: {SYMBOL}")
    print(f"  日期: {DATE}")
    print(f"  测试目标: 验证欠债清零后可以继续下主动单")
    print(f"{'='*70}")
    
    # 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }
    
    strategy = DebtClearTestStrategy(account="debt_clear_test")
    
    print(f"\n🚀 开始回测...")
    
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_debt_clear",
        strict_active_order_mode=True  # 开启严格模式
    )
    
    if success:
        print(f"\n✅ 回测完成!")
        strategy.print_summary()
    else:
        print(f"\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
