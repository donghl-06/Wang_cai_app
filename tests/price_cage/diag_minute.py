# 通用临停只次失配定位:按分钟对比 引擎 vs 真实 成交量,列出差异最大的分钟
# 用法: HALT_BASE=<dir> .venv/bin/python tests/price_cage/diag_minute.py SYM DAY
import os
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = sys.argv[1], sys.argv[2]
BASE = Path(os.environ.get("HALT_BASE", "/home/donghuale/project3/cpp/adata_logs"))


class Collector(Strategy):
    def __init__(self):
        super().__init__()
        self.trades = []

    def getStrategyId(self):
        return "DG"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == '1':
                self.trades.append((int(t.Time), int(t.Price), int(t.Volume)))
        return []


def to_sec(s):
    t = s.split()[-1]
    h, m, rest = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def main():
    files = load_one(BASE, SYM, DAY)
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    c = Collector()
    ok = run_backtest(data_dict, c, release_input=True)
    print(f"run_backtest: {ok}, 引擎成交 {len(c.trades)} 条")

    eng_min = defaultdict(int)
    for t, px, v in c.trades:
        s = t // 1000  # HHMMSS
        sec = (s // 10000) * 3600 + ((s // 100) % 100) * 60 + s % 100
        eng_min[sec // 60] += v

    real_min = defaultdict(int)
    df = files["cstra"]
    ex = df["exectype"].astype(str)
    sub = df[ex.str.contains("b'1'")]
    for r in sub.itertuples(index=False):
        real_min[int(to_sec(str(r.time).strip("'"))) // 60] += int(r.size)

    minutes = sorted(set(eng_min) | set(real_min))
    diffs = [(abs(eng_min[m] - real_min[m]), m, eng_min[m], real_min[m])
             for m in minutes]
    diffs.sort(reverse=True)
    print(f"{'分钟':>8} {'引擎量':>12} {'真实量':>12} {'差':>12}")
    for d, m, e, r in diffs[:25]:
        if d == 0:
            break
        print(f"{m//60:02d}:{m%60:02d}     {e:>12} {r:>12} {e-r:>+12}")
    tot_e, tot_r = sum(eng_min.values()), sum(real_min.values())
    print(f"总量: 引擎 {tot_e} 真实 {tot_r} 差 {tot_e - tot_r:+}")


if __name__ == "__main__":
    main()
