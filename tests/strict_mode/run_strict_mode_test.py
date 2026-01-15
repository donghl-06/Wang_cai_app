"""
运行严格主动单模式测试

使用方法:
    python run_strict_mode_test.py

功能说明:
    - 启用严格主动单模式
    - 测试主动单部分成交+剩余撤单
    - 测试欠债限制（有欠债时拒绝新的主动单）
"""

import sys
import os

# 使用本地编译的包
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../..'))

import pandas as pd
from pathlib import Path

from wangcai_syn import run_backtest
from test_strict_mode import StrictModeTestStrategy

# ==================== 关键校验：打印实际加载的本地包/扩展路径 ====================
# 这能快速确认“是否真的在用最新编译的 .so”，避免因为 Python 路径优先级导致误用安装包。
try:
    import wangcai_syn
    import wangcai_syn.wangcai_cpp as _ext
    print(f"📦 wangcai_syn 路径: {wangcai_syn.__file__}")
    print(f"📦 扩展模块路径: {_ext.__file__}")
except Exception as _e:
    print(f"⚠️ 无法打印扩展模块路径: {_e}")


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
    # 1. 加载数据
    print(f"\n📖 加载数据: {SYMBOL} @ {DATE}")
    cstick, csord, cstra, csbar1d = load_data(SYMBOL, DATE, DATA_DIR)
    
    # 2. 组织数据格式
    data = {
        SYMBOL: (cstick, csord, cstra, csbar1d)
    }
    
    # 3. 创建测试策略
    strategy = StrictModeTestStrategy(account="test_strict")
    
    # 4. 运行回测（启用严格主动单模式）
    print(f"\n{'='*60}")
    print(f"🧪 严格主动单模式测试")
    print(f"{'='*60}")
    print(f"  模式: {'✅ 开启' if ENABLE_STRICT_MODE else '❌ 关闭'}")
    print(f"  测试内容:")
    print(f"    1. 主动单部分成交+剩余撤单")
    print(f"    2. 欠债限制（有欠债时拒绝新的主动单）")
    print(f"{'='*60}\n")
    
    print(f"🚀 开始回测...")
    success = run_backtest(
        data_dict=data,
        strategy=strategy,
        output_dir=f"{OUTPUT_DIR}/{SYMBOL}_{DATE}_strict_mode",
        strict_active_order_mode=ENABLE_STRICT_MODE  # 启用严格主动单模式
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
