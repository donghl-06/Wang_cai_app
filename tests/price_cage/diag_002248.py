# 诊断:002248.SZ 2024-12-11 引擎重建成交 vs 真实成交逐笔对齐,定位首笔分歧
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_adata_validation import Collector, load_one, real_agg, is_etf_sym

from wangcai_syn import run_backtest

SYM, DAY, BASE = "002248.SZ", "2024-12-11", Path("adata_logs")


class DiagCollector(Collector):
    """同口径收集,但保留时间戳且不聚合,便于逐笔对齐"""

    def __init__(self):
        super().__init__()
        self.seq = defaultdict(list)  # sym -> [(ms, ch, buy, sell, px, vol)]

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == "1":
                hhmmss = t.Time // 1000
                if 93000 <= hhmmss < 145700:
                    # 引擎 Time 编码为 HHMMSS*1000+ms,归一化为当日毫秒
                    ms = (hhmmss // 10000 * 3600 + hhmmss // 100 % 100 * 60
                          + hhmmss % 100) * 1000 + int(t.Time) % 1000
                    self.seq[t.Instrument].append(
                        (ms, int(t.ChannelNo), int(t.BuyNo),
                         int(t.SellNo), int(t.Price), int(t.Volume)))
        return []


def norm_bytes(v):
    s = str(v)
    return s[2:-1] if s.startswith("b'") else s.strip()


def real_seq(cstra):
    ts = cstra["time"].astype(str)
    ex = cstra["exectype"].map(norm_bytes)
    df = cstra[(ex == "1") & (ts >= "0 days 09:30:00") & (ts < "0 days 14:57:00")]
    out = []
    for r in df.itertuples(index=False):
        t = str(r.time).split()[-1]
        hh, mm, rest = t.split(":")
        ss, _, ms = rest.partition(".")
        ms_key = (int(hh) * 3600 + int(mm) * 60 + int(ss)) * 1000 + int(ms[:3] or 0)
        out.append((ms_key, int(r.channelno), int(r.bidorderid),
                    int(r.askorderid), int(round(float(r.price) * 10000)),
                    int(r.size)))
    return out


def fmt(e):
    ms, ch, b, a, p, v = e
    hhmmss, msec = divmod(ms, 1000)
    return (f"{hhmmss//10000:02d}:{hhmmss//100%100:02d}:{hhmmss%100:02d}.{msec:03d}"
            f" ch{ch} {b}x{a} {p/10000:.2f} x{v}")


def main():
    files = load_one(BASE, SYM, DAY)
    real = real_seq(files["cstra"])
    data = {SYM: (files["cstick"], files["csord"], files["cstra"],
                  files["csbar1d"], is_etf_sym(SYM))}
    col = DiagCollector()
    err = None
    try:
        ok = run_backtest(data, col, n_workers=1, release_input=False)
        print(f"run_backtest ok={ok}")
    except Exception as e:
        err = e
        print(f"引擎异常: {e}")
    eng = col.seq.get(SYM, [])
    print(f"真实成交 {len(real)} 笔, 引擎已产出 {len(eng)} 笔")

    # 逐笔对齐找首笔分歧
    i = j = 0
    div = None
    while i < len(real) and j < len(eng):
        if real[i] == eng[j]:
            i += 1
            j += 1
            continue
        div = (i, j)
        break
    if div is None and i == len(real) and j == len(eng):
        print("无分歧")
        return
    i, j = div if div else (i, j)
    print(f"\n===== 首笔分歧: 真实[{i}] vs 引擎[{j}] =====")
    lo = max(0, i - 5)
    print("--- 真实前文 ---")
    for k in range(lo, min(i + 3, len(real))):
        mark = ">>" if k == i else "  "
        print(f"{mark} [{k}] {fmt(real[k])}")
    print("--- 引擎前文 ---")
    for k in range(max(0, j - 5), min(j + 3, len(eng))):
        mark = ">>" if k == j else "  "
        print(f"{mark} [{k}] {fmt(eng[k])}")
    # 分歧涉及的真实订单:取双方 orderid 查原始委托
    ids = set()
    for e in (real[i], eng[j] if j < len(eng) else real[i]):
        ids.update({e[2], e[3]})
    o = files["csord"]
    sub = o[o["orderid"].isin(ids)]
    print("--- 涉及订单的原始委托 ---")
    for r in sub.itertuples(index=False):
        print(f"  {str(r.time)[-15:]} orderid={r.orderid} side={r.side} "
              f"type={r.ordertype} px={r.price} size={r.size}")


if __name__ == "__main__":
    main()
