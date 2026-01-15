#!/usr/bin/env python3
"""
对比测试：关闭严格主动单模式

验证：当严格模式关闭时，所有主动单都应该正常成交，不会被拦截
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

from test_strict_mode_comprehensive import ComprehensiveStrictModeTest


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../../new_log"
OUTPUT_DIR = "../../user_example/output"
ENABLE_STRICT_MODE = False  # ❌ 关闭严格模式
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
    print(f"🧪 对比测试：严格主动单模式 ❌ 关闭")
    print(f"{'='*70}")
    print(f"  标的: {SYMBOL}")
    print(f"  日期: {DATE}")
    print(f"  严格模式: {'✅ 开启' if ENABLE_STRICT_MODE else '❌ 关闭'}")
    print(f"  预期: 所有主动单都应正常成交，不会被拦截")
    print(f"{'='*70}")
    
    # 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }
    
    strategy = ComprehensiveStrictModeTest(account="strict_off_test")
    
    print(f"\n🚀 开始回测...")
    
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_strict_off",
        strict_active_order_mode=ENABLE_STRICT_MODE
    )
    
    if success:
        print(f"\n✅ 回测完成!")
        
        # 检查结果
        print(f"\n{'='*70}")
        print(f"📊 对比测试结果（严格模式关闭）")
        print(f"{'='*70}")
        
        # 关闭模式时，测试2和测试3应该成交而不是被拒绝
        test2_should_fill = not strategy.results['test2_active_buy_rejected']
        test3_should_fill = not strategy.results['test3_active_sell_rejected']
        
        if test2_should_fill and test3_should_fill:
            print(f"  ✅ 严格模式关闭时，主动单没有被拦截（符合预期）")
        else:
            print(f"  ⚠️  有主动单被拦截，请检查")
            print(f"      test2_rejected: {strategy.results['test2_active_buy_rejected']}")
            print(f"      test3_rejected: {strategy.results['test3_active_sell_rejected']}")
        
        print(f"  最终持仓: {strategy.position}")
        print(f"{'='*70}")
    else:
        print(f"\n❌ 回测失败")
        return 1
    
    return 0


if __name__ == "__main__":
    exit(main())
