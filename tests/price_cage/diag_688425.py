# 688425.SH 2021-06-22 失配对账诊断:按时间段分桶统计 假阴/多出
import csv
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym, real_agg, eng_agg  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = "688425.SH", "2021-06-22"
BASE = Path("/home/donghuale/project3/cpp/adata_logs")


class Collector(Strategy):
    def __init__(self):
        super().__init__()
        self.trades = []  # (time, ch, buy, sell, px, vol)

    def getStrategyId(self):
        return "DG"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == '1':
                hhmmss = t.Time // 1000
                if 93000 <= hhmmss < 145700:
                    self.trades.append((int(t.Time), int(t.ChannelNo),
                                        int(t.BuyNo), int(t.SellNo),
                                        int(t.Price), int(t.Volume)))
        return []


def bucket(t_ms):
    """HHMMSSmmm -> 时段标签"""
    s = t_ms // 1000
    if s < 100730: return "0_开盘~临停1"
    if s < 101730: return "1_临停1空窗"
    if s < 101800: return "2_复牌1竞价"
    if s < 102253: return "3_临停1~2间"
    if s < 103253: return "4_临停2空窗"
    if s < 103300: return "5_复牌2竞价"
    if s < 113000: return "6_上午其余"
    if s < 130000: return "7_午休"
    return "8_下午"


def main():
    files = load_one(BASE, SYM, DAY)
    # 真值带时间:聚合键 -> (时段, vol)
    real = {}
    ex = files["cstra"]["exectype"].astype(str)
    ts = files["cstra"]["time"].astype(str)
    df = files["cstra"][(ex.str.contains("b'1'")) &
                        (ts >= "0 days 09:30:00") & (ts < "0 days 14:57:00")]
    real_tm = {}
    for r in df.itertuples(index=False):
        k = (int(r.channelno), int(r.bidorderid), int(r.askorderid),
             int(round(float(r.price) * 10000)))
        real[k] = real.get(k, 0) + int(r.size)
        hhmmss = int(r.time.split()[-1].replace(":", "")[:6])
        real_tm.setdefault(k, hhmmss)

    collector = Collector()
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    ok = run_backtest(data_dict, collector, release_input=True)
    print("run_backtest:", ok)

    eng = {}
    eng_tm = {}
    for t, ch, b, a, p, v in collector.trades:
        k = (ch, b, a, p)
        eng[k] = eng.get(k, 0) + v
        eng_tm.setdefault(k, t // 1000)

    fn_by, fp_by = defaultdict(int), defaultdict(int)
    fn_pairs_by, fp_pairs_by = defaultdict(int), defaultdict(int)
    for k, v in real.items():
        if eng.get(k, 0) < v:
            fn_by[bucket(real_tm[k] * 1000)] += v - eng.get(k, 0)
            fn_pairs_by[bucket(real_tm[k] * 1000)] += 1
    for k, v in eng.items():
        if k not in real:
            fp_by[bucket(eng_tm[k] * 1000)] += v
            fp_pairs_by[bucket(eng_tm[k] * 1000)] += 1
    print("假阴(真实有引擎缺) 按时段: pairs / vol")
    for b in sorted(fn_by):
        print(f"  {b}: {fn_pairs_by[b]} pairs / {fn_by[b]} 股")
    print("多出(引擎有真实无) 按时段: pairs / vol")
    for b in sorted(fp_by):
        print(f"  {b}: {fp_pairs_by[b]} pairs / {fp_by[b]} 股")

    # 复牌竞价价位分布:引擎 vs 真实 在 10:17:30 / 10:32:52 的成交量
    for lo, hi, tag in ((101730, 101800, "复牌1"), (103252, 103330, "复牌2")):
        ev = sum(v for t, ch, b, a, p, v in collector.trades
                 if lo <= t // 1000 < hi)
        print(f"{tag} 引擎成交量: {ev}")


if __name__ == "__main__":
    main()
