"""
基准测试：空策略跑 A 股回测，测试 wangcai_syn 引擎处理多只股票的耗时
可通过 MAX_SYMBOLS 控制测试股票数量
"""

import sys
import time
import glob
import os
from pathlib import Path
from typing import List
from concurrent.futures import ThreadPoolExecutor, as_completed

import pandas as pd
import wangcai_syn
from wangcai_syn import (
    Strategy, UserEvent, Snapshot,
    OrderDetail, TradeDetail,
    TradeCallback, OrderCallback,
)

DATA_DIR = "./adata_logs"
DATE = "2026-02-27"
MAX_SYMBOLS = 600
N_LOAD_WORKERS = 8
N_CONVERT_WORKERS = 8


class EmptyStrategy(Strategy):
    def __init__(self):
        super().__init__()

    def getStrategyId(self) -> str:
        return "benchmark"

    def onTickEvent(self, tick: Snapshot) -> List[UserEvent]:
        return []

    def onOrderEvent(self, order: OrderDetail) -> List[UserEvent]:
        return []

    def onTradeEvent(self, trade: TradeDetail) -> List[UserEvent]:
        return []

    def onOrderCallback(self, cb: OrderCallback) -> None:
        pass

    def onTradeCallback(self, cb: TradeCallback) -> None:
        pass


def discover_symbols(data_dir: str, date: str) -> list:
    pattern = os.path.join(data_dir, f"cstick_*_{date}.csv")
    files = glob.glob(pattern)
    symbols = []
    for f in files:
        basename = os.path.basename(f)
        sym = basename.replace("cstick_", "").replace(f"_{date}.csv", "")
        symbols.append(sym)
    symbols.sort()
    return symbols


def load_one(data_dir: str, sym: str, date: str):
    p = Path(data_dir)
    cstick = pd.read_csv(p / f"cstick_{sym}_{date}.csv")
    csord = pd.read_csv(p / f"csord_{sym}_{date}.csv")
    cstra = pd.read_csv(p / f"cstra_{sym}_{date}.csv")
    csbar1d = pd.read_csv(p / f"csbar1d_{sym}_{date}.csv")
    if len(cstick) == 0:
        raise ValueError(f"{sym} cstick is empty")
    return cstick, csord, cstra, csbar1d


def main():
    print("=" * 60)
    label = f"前 {MAX_SYMBOLS} 只" if MAX_SYMBOLS else "全 A 股"
    print(f"  wangcai_syn {label} 基准测试（空策略）")
    print("=" * 60)

    all_symbols = discover_symbols(DATA_DIR, DATE)
    sh_symbols = [s for s in all_symbols if s.endswith('.SH')]
    sz_symbols = [s for s in all_symbols if s.endswith('.SZ')]

    if MAX_SYMBOLS is not None:
        half = MAX_SYMBOLS // 2
        sh_pick = sh_symbols[:half] if len(sh_symbols) >= half else sh_symbols
        sz_pick = sz_symbols[:(MAX_SYMBOLS - len(sh_pick))]
        symbols = sh_pick + sz_pick
        symbols.sort()
        print(f"\n📋 发现 {len(all_symbols)} 只股票（SH {len(sh_symbols)}, SZ {len(sz_symbols)}）")
        print(f"   本次测试 {len(symbols)} 只（SH {len(sh_pick)}, SZ {len(sz_pick)}），日期: {DATE}")
    else:
        symbols = all_symbols
        print(f"\n📋 发现 {len(symbols)} 只股票，日期: {DATE}")

    # ---- 阶段1：加载数据（多线程） ----
    print(f"\n⏳ [阶段1] 加载数据到内存（{N_LOAD_WORKERS} 线程）...")
    t0 = time.time()
    data_dict = {}
    failed_load = []

    with ThreadPoolExecutor(max_workers=N_LOAD_WORKERS) as executor:
        future_to_sym = {executor.submit(load_one, DATA_DIR, sym, DATE): sym for sym in symbols}
        done_count = 0
        for future in as_completed(future_to_sym):
            done_count += 1
            sym = future_to_sym[future]
            try:
                data_dict[sym] = future.result()
            except Exception as e:
                failed_load.append((sym, str(e)))
            if done_count % 200 == 0 or done_count == len(symbols):
                print(f"   已加载 {done_count}/{len(symbols)}...")

    t_load = time.time() - t0
    print(f"✅ 数据加载完成: {len(data_dict)} 只成功, {len(failed_load)} 只失败")
    print(f"   耗时: {t_load:.2f} 秒")

    if failed_load:
        print(f"   失败列表（前10）: {[x[0] for x in failed_load[:10]]}")

    # 统计总行数
    total_rows = 0
    for sym, data_tuple in data_dict.items():
        cstick_df, order_df, trade_df, csbar1d_df = data_tuple[:4]
        total_rows += len(cstick_df) + len(order_df) + len(trade_df) + len(csbar1d_df)

    # ---- 阶段2：运行回测 ----
    n_symbols = len(data_dict)
    print(f"\n⏳ [阶段2] 运行回测（{n_symbols} 只股票同时跑）...")
    strategy = EmptyStrategy()
    t1 = time.time()
    success = wangcai_syn.run_backtest(
        data_dict=data_dict,
        strategy=strategy,
        n_workers=N_CONVERT_WORKERS,
        release_input=True,
    )
    t_backtest = time.time() - t1

    # ---- 结果 ----
    print(f"\n{'=' * 60}")
    print(f"  📊 基准测试结果")
    print(f"{'=' * 60}")
    print(f"  股票数量:    {n_symbols}")
    print(f"  回测总行数:  {total_rows:,}")
    print(f"  回测状态:    {'成功' if success else '失败'}")
    print(f"  数据加载耗时: {t_load:.2f} 秒")
    print(f"  回测引擎耗时: {t_backtest:.2f} 秒")
    print(f"  总耗时:      {t_load + t_backtest:.2f} 秒")
    print(f"  平均每只:    {t_backtest / n_symbols * 1000:.2f} 毫秒")
    print(f"  吞吐量:      {total_rows / t_backtest:,.0f} 行/秒")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
    sys.stdout.flush()
    os._exit(0)
