#!/usr/bin/env python3
"""
运行用户自定义数据推送测试
"""

import os
import sys
from pathlib import Path

import pandas as pd

# 确保使用本地编译的包
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../..'))

import wangcai_syn
from wangcai_syn import run_backtest

from test_custom_data import CustomDataTestStrategy

# 打印加载路径，确认本地包
print(f"📦 wangcai_syn: {wangcai_syn.__file__}")
print(f"📦 扩展模块: {wangcai_syn.wangcai_cpp.__file__}")


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../../new_log"
OUTPUT_DIR = "../../user_example/output"
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


def build_custom_data_cases():
    """
    构造多组自定义数据样例：
    - 第三列 signal 为 yes/no
    - yes -> 下单
    - no  -> 不下单
    """
    cases = []
    
    # Case 1: 基础 yes/no
    cases.append((
        "case_basic_yes_no",
        pd.DataFrame({
            "datetime": [
                "2025-11-17 09:31:00",
                "2025-11-17 09:31:30",
                "2025-11-17 09:32:00",
                "2025-11-17 10:00:00",
            ],
            "target_price": [44.30, 0.0, 44.50, 0.0],
            "signal": ["yes", "no", "yes", "no"],
        })
    ))
    
    # Case 2: 相同时间戳顺序验证
    cases.append((
        "case_same_time_order",
        pd.DataFrame({
            "datetime": [
                "2025-11-17 09:33:00",
                "2025-11-17 09:33:00",
                "2025-11-17 09:33:00",
            ],
            "target_price": [44.20, 44.25, 44.30],
            "signal": ["yes", "no", "yes"],
        })
    ))
    
    # Case 3: 全部 no（不应下单）
    cases.append((
        "case_all_no",
        pd.DataFrame({
            "datetime": [
                "2025-11-17 09:34:00",
                "2025-11-17 09:35:00",
                "2025-11-17 09:36:00",
            ],
            "target_price": [0.0, 0.0, 0.0],
            "signal": ["no", "no", "no"],
        })
    ))
    
    return cases


def main():
    print(f"\n{'='*70}")
    print("🧪 用户自定义数据推送测试")
    print(f"{'='*70}")
    print(f"  标的: {SYMBOL}")
    print(f"  日期: {DATE}")
    print(f"{'='*70}")

    # 1. 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)

    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }

    # 2. 构造自定义数据样例并逐个测试
    cases = build_custom_data_cases()
    failed_cases = []
    
    for case_name, custom_df in cases:
        print(f"\n{'-'*70}")
        print(f"📌 测试样例: {case_name}")
        print(custom_df)
        print(f"{'-'*70}")
        
        # 创建策略
        strategy = CustomDataTestStrategy(account=f"custom_{case_name}")
        
        # 运行回测
        print("\n🚀 开始回测...")
        success = run_backtest(
            data_dict=data,
            strategy=strategy,
            output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_custom_data_{case_name}",
            custom_data=custom_df,
            enable_custom_data=True
        )
        
        if success:
            print("\n✅ 回测完成!")
            strategy.print_summary()
            if not strategy.passed():
                failed_cases.append(case_name)
        else:
            print("\n❌ 回测失败")
            return 1

    if failed_cases:
        print(f"\n❌ 未收到自定义数据的样例: {failed_cases}")
        return 1
    return 0


if __name__ == "__main__":
    exit(main())
