#!/usr/bin/env python3
"""
运行严格主动单模式全面测试
"""

import os
import sys
from pathlib import Path

import pandas as pd

# 确保使用本地编译的包
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../..'))

import wangcai_syn
from wangcai_syn import run_backtest

# 验证加载的包路径
print(f"📦 wangcai_syn: {wangcai_syn.__file__}")
print(f"📦 扩展模块: {wangcai_syn.wangcai_cpp.__file__}")

from test_strict_mode_comprehensive import ComprehensiveStrictModeTest


# ========== 配置区 ==========
SYMBOL = "300827.SZ"          # 股票代码
DATE = "2025-11-17"           # 回测日期
DATA_DIR = "../../new_log"       # 数据目录
OUTPUT_DIR = "../../user_example/output"       # 输出目录
ENABLE_STRICT_MODE = True     # 是否启用严格主动单模式
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
    print(f"\n{'='*70}")
    print(f"🧪 严格主动单模式 - 全面测试")
    print(f"{'='*70}")
    print(f"  标的: {SYMBOL}")
    print(f"  日期: {DATE}")
    print(f"  严格模式: {'✅ 开启' if ENABLE_STRICT_MODE else '❌ 关闭'}")
    print(f"{'='*70}")
    
    # 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    # 组织数据格式
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }
    
    # 创建测试策略
    strategy = ComprehensiveStrictModeTest(account="comprehensive_test")
    
    # 运行回测
    print(f"\n🚀 开始回测...")
    
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_comprehensive",
        strict_active_order_mode=ENABLE_STRICT_MODE
    )
    
    if success:
        print(f"\n✅ 回测完成!")
        # 打印测试结果
        strategy.print_summary()
    else:
        print(f"\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
