# 通用临停只次对账:配对级 假阴/多出 按临停窗口分桶 + 最劣桶内 top 失配明细
# 用法: HALT_BASE=<dir> .venv/bin/python tests/price_cage/diag_pairs.py SYM DAY
import csv
import os
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = sys.argv[1], sys.argv[2]
BASE = Path(os.environ.get("HALT_BASE", "/home/donghuale/project3/cpp/adata_logs"))


def to_sec(s):
    t = s.split()[-1]
    h, m, rest = t.split(":")
    return int(h) * 3600 + int(m) * 60 + float(rest)


def fmt(sec):
    return f"{int(sec//3600):02d}:{int(sec%3600//60):02d}:{sec%60:04.1f}"


def real_windows():
    ts = []
    with open(BASE / f"cstra_{SYM}_{DAY}.csv") as f:
        for r in csv.DictReader(f):
            if "b'1'" in r["exectype"]:
                ts.append(to_sec(r["time"].strip("'")))
    ts.sort()
    wins = []
    for a, b in zip(ts, ts[1:]):
        in_session = (34200 <= a < 41400) or (46800 <= a < 53700)
        if b - a > 180 and in_session:
            wins.append((a, b))
    return wins


class Collector(Strategy):
    def __init__(self):
        super().__init__()
        self.trades = []

    def getStrategyId(self):
        return "DG"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == '1':
                self.trades.append((int(t.Time), int(t.ChannelNo),
                                    int(t.BuyNo), int(t.SellNo),
                                    int(t.Price), int(t.Volume)))
        return []


def main():
    wins = real_windows()
    print(f"真实空窗: {[(fmt(a), fmt(b)) for a, b in wins]}")

    # 分桶: 窗口内 / 窗口后 2 分钟 / 其余
    def bucket(sec):
        for i, (a, b) in enumerate(wins):
            if a - 1 <= sec <= b + 1:
                return f"临停{i+1}窗内 {fmt(a)}~{fmt(b)}"
            if b + 1 < sec <= b + 120:
                return f"临停{i+1}复牌后2分"
        return "其余"

    files = load_one(BASE, SYM, DAY)
    df = files["cstra"]
    ex = df["exectype"].astype(str)
    ts = df["time"].astype(str)
    # 与校验口径一致:只覆盖 [09:30:00, 14:57:00) 连续竞价段
    sub = df[ex.str.contains("b'1'") & (ts >= "0 days 09:30:00")
             & (ts < "0 days 14:57:00")]
    real, real_tm = {}, {}
    for r in sub.itertuples(index=False):
        k = (int(r.channelno), int(r.bidorderid), int(r.askorderid),
             int(round(float(r.price) * 10000)))
        real[k] = real.get(k, 0) + int(r.size)
        real_tm.setdefault(k, to_sec(str(r.time).strip("'")))

    c = Collector()
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    ok = run_backtest(data_dict, c, release_input=True)
    print("run_backtest:", ok, "引擎成交", len(c.trades))

    eng, eng_tm = {}, {}
    for t, ch, b, a, p, v in c.trades:
        hhmmss = t // 1000
        if not (93000 <= hhmmss < 145700):  # 校验口径:连续竞价段
            continue
        sec = (hhmmss // 10000) * 3600 + ((hhmmss // 100) % 100) * 60 + hhmmss % 100
        k = (ch, b, a, p)
        eng[k] = eng.get(k, 0) + v
        eng_tm.setdefault(k, sec)

    fn_by, fp_by = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
    fn_detail, fp_detail = [], []
    for k, v in real.items():
        d = v - eng.get(k, 0)
        if d > 0:
            bk = bucket(real_tm[k])
            fn_by[bk][0] += 1
            fn_by[bk][1] += d
            fn_detail.append((d, real_tm[k], k))
    for k, v in eng.items():
        d = v - real.get(k, 0)
        if d > 0:
            bk = bucket(eng_tm[k])
            fp_by[bk][0] += 1
            fp_by[bk][1] += d
            fp_detail.append((d, eng_tm[k], k))
    print("假阴(真实有引擎缺): 桶 pairs/vol")
    for b in sorted(fn_by):
        print(f"  {b}: {fn_by[b][0]} pairs / {fn_by[b][1]} 股")
    print("多出(引擎有真实无):")
    for b in sorted(fp_by):
        print(f"  {b}: {fp_by[b][0]} pairs / {fp_by[b][1]} 股")

    fn_detail.sort(reverse=True)
    fp_detail.sort(reverse=True)
    print("\n假阴 top10: vol time (ch,buy,sell,px)")
    for d, t, k in fn_detail[:10]:
        print(f"  {d:>8} {fmt(t)} {k}")
    print("多出 top10:")
    for d, t, k in fp_detail[:10]:
        print(f"  {d:>8} {fmt(t)} {k}")

    # 与校验口径一致:纯键集差(引擎量为 0 的真实键 / 真实没有的引擎键)
    pure_fn = [(real_tm[k], k, real[k]) for k in real if eng.get(k, 0) == 0]
    pure_fp = [(eng_tm[k], k, eng[k]) for k in eng if k not in real]
    print(f"\n纯假阴键 {len(pure_fn)} 个 / 纯多出键 {len(pure_fp)} 个")
    for t, k, v in sorted(pure_fn)[:20]:
        print(f"  FN {fmt(t)} {k} vol={v}")
    for t, k, v in sorted(pure_fp)[:20]:
        print(f"  FP {fmt(t)} {k} vol={v}")

    # 各空窗边界(触发笔/复牌打印)的 价位×量 分布对比
    def hist(trades_iter, a, b):
        h = defaultdict(int)
        for px, v in trades_iter:
            h[px] += v
        return dict(h)
    for i, (a, b) in enumerate(wins):
        ra, rb = defaultdict(int), defaultdict(int)
        for r in sub.itertuples(index=False):
            s = to_sec(str(r.time).strip("'"))
            px = int(round(float(r.price) * 10000))
            if a - 1 <= s <= a + 0.5:
                ra[px] += int(r.size)
            if b - 0.5 <= s <= b + 1:
                rb[px] += int(r.size)
        ea, eb = defaultdict(int), defaultdict(int)
        for t, ch, bb, aa, p, v in c.trades:
            hhmmss = t // 1000
            s = (hhmmss // 10000) * 3600 + ((hhmmss // 100) % 100) * 60 + hhmmss % 100
            if a - 1 <= s <= a + 0.5:
                ea[p] += v
            if b - 0.5 <= s <= b + 1:
                eb[p] += v
        print(f"\n空窗{i+1} 触发端 {fmt(a)}: 真实 {sorted(ra.items())} | 引擎 {sorted(ea.items())}")
        print(f"空窗{i+1} 复牌端 {fmt(b)}: 真实 {sorted(rb.items())} | 引擎 {sorted(eb.items())}")


if __name__ == "__main__":
    main()
