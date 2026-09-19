"""301408.SZ 2023-03-01 engine_crash 最小复现:同进程直跑,崩溃信号直接可见。"""
import faulthandler
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tests" / "price_cage"))

faulthandler.enable()

from run_adata_validation import Collector, is_etf_sym, load_one  # noqa: E402
from wangcai_syn import run_backtest  # noqa: E402

base = ROOT / "adata_logs"
sym, day = "301408.SZ", "2023-03-01"

files = load_one(base, sym, day)
print("数据加载完成,进引擎...", flush=True)
data_dict = {sym: (files["cstick"], files["csord"], files["cstra"],
                   files["csbar1d"], is_etf_sym(sym))}
collector = Collector()
ok = run_backtest(data_dict, collector, release_input=True)
print(f"run_backtest 返回 {ok}, 成交 {len(collector.trades.get(sym, []))} 条", flush=True)
