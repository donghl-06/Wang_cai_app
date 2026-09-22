# 新股首日临停验证 trace:统计引擎在真实临停窗口内产生的成交
# 用法: .venv/bin/python tests/price_cage/trace_halt.py 688425.SH 2021-06-22 [起 止]
#   起/止 为 HHMMSS 整数,默认自动从真实成交流探测 >3min 的日间空窗
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = sys.argv[1], sys.argv[2]


def to_sec(s):  # "0 days 10:17:30.120000" -> 秒
    t = s.split()[-1]
    h, m, rest = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def real_windows(base):
    """真实成交流(exectype=1)中 >3min 的日间空窗(剔除午休与收市)"""
    ts = []
    import csv
    with open(base / f"cstra_{SYM}_{DAY}.csv") as f:
        for r in csv.DictReader(f):
            if "b'1'" in r["exectype"]:  # 字段带引号: '"b\'1\'"'
                ts.append(to_sec(r["time"].strip("'")))
    ts.sort()
    wins = []
    for a, b in zip(ts, ts[1:]):
        in_session = (34200 <= a < 41400) or (46800 <= a < 53700)  # 上/下午连续竞价
        if b - a > 180 and in_session:
            wins.append((a, b))
    return wins


class Tracer(Strategy):
    def __init__(self):
        super().__init__()
        self.trades = []

    def getStrategyId(self):
        return "TR"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            self.trades.append((int(t.Time), int(t.Price), int(t.Volume),
                                int(t.BuyNo), int(t.SellNo)))
        return []


def main():
    import os
    base = Path(os.environ.get("HALT_BASE", "/home/donghuale/project3/cpp/adata_logs"))
    wins = real_windows(base)
    if len(sys.argv) > 4:
        s0 = sys.argv[3]; s1 = sys.argv[4]
        w0 = int(s0[:2]) * 3600 + int(s0[2:4]) * 60 + int(s0[4:])
        w1 = int(s1[:2]) * 3600 + int(s1[2:4]) * 60 + int(s1[4:])
        wins = [(w0, w1)]
    print(f"真实临停空窗: {[(f'{int(a//3600):02d}:{int(a%3600//60):02d}:{a%60:04.1f}', f'{int(b//3600):02d}:{int(b%3600//60):02d}:{b%60:04.1f}') for a, b in wins]}")

    files = load_one(base, SYM, DAY)
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    tr = Tracer()
    ok = run_backtest(data_dict, tr, release_input=True)
    print(f"run_backtest: {ok}, 引擎成交 {len(tr.trades)} 条")

    for a, b in wins:
        t0 = int(a % 3600 // 60) * 100000 + int(a % 60) * 1000 + int(a // 3600) * 10000000
        # Time 格式 HHMMSSmmm(int),直接构造
        def hhmmssmmm(sec):
            h = int(sec // 3600); m = int(sec % 3600 // 60); s = sec % 60
            return h * 10000000 + m * 100000 + int(s) * 1000 + int((s - int(s)) * 1000)
        lo, hi = hhmmssmmm(a), hhmmssmmm(b)
        in_win = [t for t in tr.trades if lo <= t[0] <= hi]
        vol = sum(t[2] for t in in_win)
        print(f"  空窗 {hhmmssmmm(a)}~{hhmmssmmm(b)}: 引擎成交 {len(in_win)} 条 / {vol} 股")
        for t in in_win[:5]:
            print(f"    {t}")


if __name__ == "__main__":
    main()
