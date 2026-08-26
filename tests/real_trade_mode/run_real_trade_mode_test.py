"""
运行真实成交替代模式测试

使用方法:
    python run_real_trade_mode_test.py

功能说明:
    - 启用 real_trade_match_mode
    - 测试被动单排队成交（队列位置+真实成交池）
    - 测试主动单自动走严格模式
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../..'))

import pandas as pd
from pathlib import Path

from wangcai_syn import run_backtest
from test_real_trade_mode import RealTradeMatchTestStrategy

# 打印包路径用于调试
try:
    import wangcai_syn
    import wangcai_syn.wangcai_cpp as _ext
    print(f"wangcai_syn 路径: {wangcai_syn.__file__}")
    print(f"扩展模块路径: {_ext.__file__}")
except Exception as _e:
    print(f"无法打印扩展模块路径: {_e}")


# ========== 配置区 ==========
SYMBOL = "300827.SZ"
DATE = "2025-11-17"
DATA_DIR = "../../new_log"
OUTPUT_DIR = "./output"
# ============================


def load_data(symbol: str, date: str, data_dir: str):
    path = Path(data_dir)
    cstick = pd.read_csv(path / f"cstick_{symbol}_{date}.csv")
    csord = pd.read_csv(path / f"csord_{symbol}_{date}.csv")
    cstra = pd.read_csv(path / f"cstra_{symbol}_{date}.csv")
    csbar1d = pd.read_csv(path / f"csbar1d_{symbol}_{date}.csv")
    print(f"数据加载完成: {symbol}")
    print(f"  Tick: {len(cstick)} | 委托: {len(csord)} | 成交: {len(cstra)}")
    return cstick, csord, cstra, csbar1d


def main():
    print(f"\n加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    data = { SYMBOL: (cstick, csord, cstra, csbar1d) }
    
    strategy = RealTradeMatchTestStrategy(account="test_rt")
    
    print(f"\n{'='*60}")
    print(f"真实成交替代模式测试")
    print(f"{'='*60}")
    print(f"  测试内容:")
    print(f"    1. 被动买单排队成交（队列位置+真实成交池）")
    print(f"    2. 主动买单走严格模式（限于可用量+欠债）")
    print(f"{'='*60}\n")
    
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=OUTPUT_DIR,
        real_trade_match_mode=True  # 启用真实成交替代模式
    )
    
    if success:
        print(f"\n回测完成!")
        strategy.print_summary()
    else:
        print(f"\n回测失败")
        return 1
    
    if not strategy.passed():
        print(f"\n核心断言未通过：主动单既未成交也未撤单")
        return 1
    return 0


if __name__ == "__main__":
    exit(main())
