"""性能基准:单票单日计时,用于优化前后对比。

用法(项目根目录,项目 .venv):
    .venv/bin/python tests/perf/bench_single.py [--runs 3]

输出两类耗时(均含 to_csv 转换 + 引擎构建 + 撮合):
- empty:    空策略(所有事件回调空实现) → 衡量引擎+pybind 边界固定开销
- collector: 收集全部重建成交的策略(同 run_adata_validation 口径) → 衡量真实负载

基准标的:
- 000001.SZ 2024-11-01(深主板,adata_logs,~23 万事件)
- 300827.SZ 2025-11-17(创业板,new_log,±20% 笼子桶更多,集合竞价更重)
"""

import argparse
import os
import sys
import time
from collections import defaultdict
from pathlib import Path

import pandas as pd

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT))

from wangcai_syn import Strategy, run_backtest  # noqa: E402
from wangcai_syn.utils import _normalize_table_layout  # noqa: E402

CASES = [
    ("000001.SZ", "2024-11-01", ROOT / "adata_logs"),
    ("300827.SZ", "2025-11-17", ROOT / "new_log"),
]


def load(base: Path, sym: str, day: str):
    files = {k: pd.read_csv(base / f"{k}_{sym}_{day}.csv")
             for k in ("cstick", "csord", "cstra")}
    bar = base / f"csbar1d_{sym}_{day}.csv"
    if bar.is_file() and bar.stat().st_size > 0:
        bdf = pd.read_csv(bar)
    else:
        prev = float(files["cstick"]["prevclose"].dropna().iloc[-1])
        bdf = pd.DataFrame([{
            "sym": sym, "prevclose": prev, "open": prev, "high": prev,
            "low": prev, "close": prev, "volume": 0, "turnover": 0,
            "tradecount": 0, "af": 1.0,
            "upperlimit": prev * 1.2, "lowerlimit": prev * 0.8}])
    for k in ("cstick", "csord", "cstra"):
        files[k] = _normalize_table_layout(files[k], k)
    return files, bdf


class Noop(Strategy):
    """完全不覆写任何事件回调:验证覆写探测缓存(P0)的收益"""
    def getStrategyId(self): return "RW"


class Empty(Strategy):
    def getStrategyId(self): return "RW"
    def onOrderEvent(self, o): return []
    def onTradeEvent(self, t): return []
    def onTickEvent(self, s): return []
    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass
    def onOrderCallback(self, cb): pass
    def onTradeCallback(self, cb): pass


class Collector(Strategy):
    """真实负载:批量接收重建成交(onTradeEventsBatch)+ 不覆写无关回调"""
    def __init__(self):
        super().__init__()
        self.trades = defaultdict(list)

    def getStrategyId(self): return "RW"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == '1':
                hhmmss = t.Time // 1000
                if 93000 <= hhmmss < 145700:
                    self.trades[t.Instrument].append(
                        (int(t.ChannelNo), int(t.BuyNo), int(t.SellNo),
                         int(t.Price), int(t.Volume)))
        return []


def bench(sym, day, base, strategy_factory, runs):
    files, bdf = load(base, sym, day)
    times = []
    for _ in range(runs):
        t0 = time.time()
        ok = run_backtest({sym: (files["cstick"], files["csord"], files["cstra"],
                                 bdf, False)}, strategy_factory(), n_workers=1)
        times.append(time.time() - t0)
        if not ok:
            raise RuntimeError(f"{sym} 回测失败")
    return min(times), sum(times) / len(times)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    print(f"=== bench tag={args.tag or '-'} runs={args.runs} ===")
    for sym, day, base in CASES:
        if not (base / f"csord_{sym}_{day}.csv").is_file():
            print(f"{sym} {day}: 数据缺失,跳过 ({base})")
            continue
        for name, factory in (("noop", Noop), ("empty", Empty),
                              ("collector", Collector)):
            best, avg = bench(sym, day, base, factory, args.runs)
            print(f"{sym} {day} {name:9s} best={best:6.2f}s avg={avg:6.2f}s",
                  flush=True)
    sys.stdout.flush()
    os._exit(0)  # 绕过 C++ 引擎析构卡死


if __name__ == "__main__":
    main()
