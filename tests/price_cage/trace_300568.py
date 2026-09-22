# 300568.SZ 2022-06-29 单票引擎 trace:盯关键委托单的引擎侧生命周期
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import load_one, is_etf_sym  # noqa: E402
from wangcai_syn import Strategy, run_backtest  # noqa: E402

SYM, DAY = "300568.SZ", "2022-06-29"
IDS = {16121963, 17697508, 17701112, 16921658, 16923822, 17919623, 17929821}
# 时间窗口:10:17:40 ~ 10:19:00 的全部引擎成交(上下文)
T0, T1 = 101740000, 101900000


class Tracer(Strategy):
    def __init__(self):
        super().__init__()
        self.lines = []

    def getStrategyId(self):
        return "TR"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            key = int(t.BuyNo) in IDS or int(t.SellNo) in IDS
            win = T0 <= int(t.Time) < T1
            if key or (win and t.ExecType == '1'):
                self.lines.append(
                    f"TRADE {t.Time} ex={t.ExecType} "
                    f"buy={int(t.BuyNo)} sell={int(t.SellNo)} "
                    f"px={int(t.Price)} vol={int(t.Volume)} ch={int(t.ChannelNo)}")
        return []

    def onOrderEvent(self, ev):
        # 委托状态回调:入笼/出笼/撤单等(字段名按引擎实际 API)
        oid = int(getattr(ev, "OrderNo", getattr(ev, "OrderID", -1)))
        if oid in IDS:
            self.lines.append(f"ORDER {ev} ")
        return []


def main():
    base = Path("/home/donghuale/project3/cpp/adata_logs")
    files = load_one(base, SYM, DAY)
    data_dict = {SYM: (files["cstick"], files["csord"], files["cstra"],
                       files["csbar1d"], is_etf_sym(SYM))}
    tr = Tracer()
    ok = run_backtest(data_dict, tr, release_input=True)
    print("run_backtest:", ok)
    for ln in sorted(tr.lines):
        print(ln)


if __name__ == "__main__":
    main()
