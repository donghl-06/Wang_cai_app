# 688425.SH 复牌1竞价 引擎 vs 真实 逐对对比:找出配对差异形态
import csv
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = "688425.SH", "2021-06-22"
BASE = Path("/home/donghuale/project3/cpp/adata_logs")
AUCTION_T = 101730000  # 引擎复牌1竞价成交时间戳


class Collector(Strategy):
    def __init__(self):
        super().__init__()
        self.auction = defaultdict(int)   # (buy,sell)->vol @ 10:17:30.000
        self.after = defaultdict(int)     # (buy,sell,px)->vol @ 10:17:30.001~10:18:00

    def getStrategyId(self):
        return "DG2"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType != '1':
                continue
            if int(t.Time) == AUCTION_T:
                self.auction[(int(t.BuyNo), int(t.SellNo))] += int(t.Volume)
            elif AUCTION_T < int(t.Time) < 101800000:
                self.after[(int(t.BuyNo), int(t.SellNo), int(t.Price))] += int(t.Volume)
        return []


def main():
    real = defaultdict(int)
    with open(BASE / f"cstra_{SYM}_{DAY}.csv") as f:
        for r in csv.DictReader(f):
            t = r["time"].strip("'")
            if "b'1'" in r["exectype"] and t.startswith("0 days 10:17:30.12"):
                real[(int(r["bidorderid"]), int(r["askorderid"]))] += int(float(r["size"]))

    files = load_one(BASE, SYM, DAY)
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    c = Collector()
    ok = run_backtest(data_dict, c, release_input=True)
    eng = c.auction
    print("run_backtest:", ok)
    print(f"引擎复牌1: pairs={len(eng)} 总量={sum(eng.values())}")
    print(f"真实复牌1: pairs={len(real)} 总量={sum(real.values())}")

    fn = [(k, v, eng.get(k, 0)) for k, v in real.items() if eng.get(k, 0) < v]
    fp = [(k, v) for k, v in eng.items() if k not in real]
    print(f"假阴 pairs={len(fn)} 量={sum(v - e for _, v, e in fn)}")
    print(f"多出 pairs={len(fp)} 量={sum(v for _, v in fp)}")

    # 买单侧聚合:真实有而引擎没匹配的买单 / 引擎多匹配的买单
    fn_buy = defaultdict(int)
    for (b, s), v, e in fn:
        fn_buy[b] += v - e
    fp_buy = defaultdict(int)
    for (b, s), v in fp:
        fp_buy[b] += v
    print("\n假阴买单 top10 (real有eng缺):", sorted(fn_buy.items(), key=lambda x: -x[1])[:10])
    print("多出买单 top10 (eng有real无):", sorted(fp_buy.items(), key=lambda x: -x[1])[:10])
    fn_sell = defaultdict(int)
    for (b, s), v, e in fn:
        fn_sell[s] += v - e
    fp_sell = defaultdict(int)
    for (b, s), v in fp:
        fp_sell[s] += v
    print("假阴卖单 top10:", sorted(fn_sell.items(), key=lambda x: -x[1])[:10])
    print("多出卖单 top10:", sorted(fp_sell.items(), key=lambda x: -x[1])[:10])


if __name__ == "__main__":
    main()
