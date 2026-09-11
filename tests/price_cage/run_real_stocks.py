"""多只真实股票批量验证：价格笼子重建成交一致性 + 事件快照对齐率。

用法（项目根目录）:
    .venv/bin/python tests/price_cage/run_real_stocks.py            # 手工清单
    .venv/bin/python tests/price_cage/run_real_stocks.py --all      # 扫描全部数据目录
    .venv/bin/python tests/price_cage/run_real_stocks.py 688516.SH  # 指定

指标（沿用 test_real_300026 / test_real_2025 两个既有口径）:
1. 连续段(09:30–14:57)重建成交订单对一致性:
   (channelno, bidorderid, askorderid, price) 聚合量 vs 真实 cstra
   - 完全一致 / 子集匹配率(引擎量>=真实量) / 假阴(真实有引擎缺) / 假阳(引擎多出)
2. 事件快照 vs 官方 cstick 的 bid1/ask1 价格对齐率(T+1s 窗口,参考值)。
"""

import re
import sys
import time
from collections import defaultdict
from pathlib import Path

import pandas as pd

from wangcai_syn import Strategy, run_backtest
from wangcai_syn.utils import _normalize_table_layout

ROOT = Path(__file__).resolve().parent.parent.parent
NEW_LOG = ROOT / "new_log"                      # 前缀式: csord_{sym}_{date}.csv
DATA_DIR2 = ROOT / "data"                       # 前缀式（2026-09-11 新增数据集）
AQS_DIR = Path(__file__).resolve().parent / "data"   # 后缀式: {sym}_{date}_csord.csv

# (sym, date, 板块·时代说明, 是否 ETF)   时代: B=2019.7科创板开市 C=2020.8创业板暂存
#                                        D=2023.4.10 起全板块拒单(2%∨0.1)
STOCKS = [
    # --- 笼子板块·创业板：暂存窗口(StageC 纯2%) vs 拒单(StageD) ---
    ("300557.SZ", "2021-03-26", "创业板·暂存窗口(纯2%)", False),
    ("300557.SZ", "2021-11-30", "创业板·暂存窗口(纯2%)", False),
    ("300557.SZ", "2023-07-27", "创业板·拒单时代(2%∨0.1)", False),
    ("300557.SZ", "2024-10-16", "创业板·拒单时代(2%∨0.1)", False),
    ("300432.SZ", "2025-11-17", "创业板·拒单时代(2%∨0.1)", False),
    ("300502.SZ", "2025-12-15", "创业板·拒单时代(2%∨0.1)", False),
    ("300827.SZ", "2025-11-17", "创业板·拒单时代(2%∨0.1)", False),
    ("300026.SZ", "2022-06-30", "创业板·暂存窗口(纯2%)", False),
    # --- 笼子板块·科创板(开市起拒单) ---
    ("688503.SH", "2025-11-17", "科创板·拒单时代(2%∨0.1)", False),
    ("688516.SH", "2025-11-17", "科创板·拒单时代(2%∨0.1)", False),
    # --- 主板：2023.4.10 生效线两侧（StageC 无笼 → StageD 拒单） ---
    ("000027.SZ", "2022-01-07", "深主板·无笼时代", False),
    ("000027.SZ", "2023-04-06", "深主板·无笼时代(生效前4天)", False),
    ("000027.SZ", "2025-08-01", "深主板·拒单时代", False),
    ("002156.SZ", "2021-03-12", "深主板·无笼时代", False),
    ("002156.SZ", "2023-04-06", "深主板·无笼时代(生效前4天)", False),
    ("002156.SZ", "2023-07-27", "深主板·拒单时代", False),
    ("600373.SH", "2022-08-04", "沪主板·无笼时代", False),
    ("600373.SH", "2023-09-19", "沪主板·拒单时代", False),
    ("600500.SH", "2024-08-01", "沪主板·拒单时代", False),
    ("002929.SZ", "2025-12-15", "深主板·拒单时代(基线)", False),
    ("600105.SH", "2025-12-15", "沪主板·拒单时代(基线)", False),
    # --- 基金/ETF：全程无笼（对照） ---
    ("588050.SH", "2021-03-08", "科创ETF·无笼(基线)", True),
    ("588050.SH", "2023-08-07", "科创ETF·无笼(基线)", True),
    ("512010.SH", "2023-08-09", "LOF·无笼(基线)", True),
    ("159998.SZ", "2024-08-30", "创业板ETF·无笼(基线)", True),
    ("510050.SH", "2025-11-17", "ETF·全程无笼(基线)", True),
]


def _norm_bytes(v):
    t = str(v)
    return t[2:-1] if t.startswith("b'") else t.strip()


def _ms(t):
    m = re.search(r"(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?", str(t))
    hh, mm, ss = int(m.group(1)), int(m.group(2)), int(m.group(3))
    msec = int(m.group(4).ljust(3, "0")) if m.group(4) else 0
    return (hh * 3600 + mm * 60 + ss) * 1000 + msec


def load_files(sym, date):
    """new_log/data 前缀式优先,缺 csbar1d 用 cstick 合成;300026 走 aqsnapshots 后缀式。"""
    base = next((d for d in (NEW_LOG, DATA_DIR2)
                 if (d / f"csord_{sym}_{date}.csv").is_file()), None)
    if base is not None:
        files = {k: pd.read_csv(base / f"{k}_{sym}_{date}.csv")
                 for k in ("cstick", "csord", "cstra")}
        bar = base / f"csbar1d_{sym}_{date}.csv"
        if bar.is_file():
            files["csbar1d"] = pd.read_csv(bar)
        else:
            prev = float(files["cstick"]["prevclose"].dropna().iloc[-1])
            files["csbar1d"] = pd.DataFrame([{
                "sym": sym, "prevclose": prev, "open": prev, "high": prev,
                "low": prev, "close": prev, "volume": 0, "turnover": 0,
                "tradecount": 0, "af": 1.0,
                "upperlimit": prev * 1.2, "lowerlimit": prev * 0.8}])
    else:
        base = AQS_DIR
        files = {k: pd.read_csv(base / f"{sym}_{date}_{k}.csv")
                 for k in ("cstick", "csord", "cstra")}
        prev = float(files["cstick"]["prevclose"].dropna().iloc[-1])
        files["csbar1d"] = pd.DataFrame([{
            "sym": sym, "prevclose": prev, "open": prev, "high": prev,
            "low": prev, "close": prev, "volume": 0, "turnover": 0,
            "tradecount": 0, "af": 1.0,
            "upperlimit": prev * 1.2, "lowerlimit": prev * 0.8}])
    # 脚本侧断言也用 date/time 列（datetime 单列源归一化，标准源幂等无操作）
    for k, kind in (("cstick", "cstick"), ("csord", "csord"), ("cstra", "cstra")):
        files[k] = _normalize_table_layout(files[k], kind)
    return files


class Collector(Strategy):
    """连续段重建成交(ExecType=1) + 事件快照 bid1/ask1 序列"""

    def __init__(self):
        super().__init__()
        self.trades = []
        self.px_pairs = []

    def getStrategyId(self):
        return "RW"

    def onOrderEvent(self, o): return []

    def onTradeEvent(self, t):
        if t.ExecType == '1':
            hhmmss = t.Time // 1000  # HHMMSS
            if 93000 <= hhmmss < 145700:  # 仅连续竞价段（排除集合竞价 settle）
                self.trades.append((int(t.ChannelNo), int(t.BuyNo), int(t.SellNo),
                                    int(t.Price), int(t.Volume)))
        return []

    def onTickEvent(self, s): return []

    def onEventSnapshot(self, snap):
        self.px_pairs.append((snap.datetime,
                              snap.bids[0] if snap.bids else 0,
                              snap.asks[0] if snap.asks else 0))
        return []

    def onOrderFilled(self, *a): pass
    def onOrderCancelled(self, *a): pass
    def onOrderCallback(self, cb): pass
    def onTradeCallback(self, cb): pass


def real_agg(cstra):
    ts = cstra["time"].astype(str)
    ex = cstra["exectype"].map(_norm_bytes)
    df = cstra[(ex == "1") & (ts >= "0 days 09:30:00") & (ts < "0 days 14:57:00")]
    agg = {}
    for _, r in df.iterrows():
        k = (int(r["channelno"]), int(r["bidorderid"]), int(r["askorderid"]),
             int(round(float(r["price"]) * 10000)))
        agg[k] = agg.get(k, 0) + int(r["size"])
    return agg


def snapshot_align(cstick, px_pairs):
    import bisect
    snap_ms = [_ms(t) for t, _, _ in px_pairs]
    lo, hi = (9 * 3600 + 30 * 60) * 1000, (14 * 3600 + 57 * 60) * 1000
    n = px = 0
    for _, row in cstick.iterrows():
        t_ms = _ms(row["time"])
        if not (lo <= t_ms < hi):
            continue
        ob, oa = float(row["bid1"]), float(row["ask1"])
        if ob <= 0 or oa <= 0:
            continue
        idx = bisect.bisect_right(snap_ms, t_ms + 1000) - 1  # T+1s 对齐
        if idx < 0:
            continue
        _, eb, ea = px_pairs[idx]
        n += 1
        if eb == int(round(ob * 10000)) and ea == int(round(oa * 10000)):
            px += 1
    return n, px


def run_one(sym, date, note, is_etf):
    t0 = time.time()
    files = load_files(sym, date)
    s = Collector()
    assert run_backtest({sym: (files["cstick"], files["csord"], files["cstra"],
                               files["csbar1d"], is_etf)}, s,
                        event_snapshot_enabled=True)

    eng = {}
    for ch, b, a, p, v in s.trades:
        eng[(ch, b, a, p)] = eng.get((ch, b, a, p), 0) + v
    real = real_agg(files["cstra"])

    matched = sum(1 for k, v in real.items() if eng.get(k, 0) >= v)
    false_neg = [(k, v, eng.get(k, 0)) for k, v in real.items() if eng.get(k, 0) < v]
    false_pos = [(k, v) for k, v in eng.items() if k not in real]
    exact = (eng == real)
    rate = matched / max(len(real), 1)

    n_snap, px_ok = snapshot_align(files["cstick"], s.px_pairs)
    align = px_ok / max(n_snap, 1)
    dt = time.time() - t0

    print(f"\n=== {sym} {date} [{note}] ===")
    print(f"  成交一致性: {'完全一致' if exact else '有差异'} | "
          f"子集匹配率 {matched}/{len(real)} = {rate*100:.2f}% | "
          f"假阴 {len(false_neg)} | 引擎多出订单对 {len(false_pos)}")
    for k, rv, ev in false_neg[:5]:
        print(f"    [缺] ch={k[0]} buy={k[1]} sell={k[2]} px={k[3]/10000:.4f} "
              f"真实量={rv} 引擎量={ev}")
    for k, v in false_pos[:5]:
        print(f"    [多] ch={k[0]} buy={k[1]} sell={k[2]} px={k[3]/10000:.4f} 量={v}")
    print(f"  快照对齐(T+1s bid1/ask1 价格): {px_ok}/{n_snap} = {align*100:.2f}%")
    print(f"  耗时 {dt:.1f}s")
    return {"sym": sym, "date": date, "note": note, "exact": exact,
            "rate": rate, "fn": len(false_neg), "fp": len(false_pos),
            "align": align, "n_snap": n_snap, "sec": dt}


def _board_era_desc(code: str, mkt: str, date: str) -> str:
    """按代码前缀+日期生成板块·时代说明（时代界线与 price_cage.h stageOf 一致）。"""
    if (mkt == 'SH' and code.startswith('5')) or (mkt == 'SZ' and code.startswith('1')):
        return '基金·全程无笼'
    if code.startswith(('300', '301')):
        board = '创业板'
    elif code.startswith(('688', '689')):
        board = '科创板'
    elif mkt == 'SZ':
        board = '深主板'
    else:
        board = '沪主板'
    if board == '科创板' and date < '2019-07-22':
        era = '开市前(不适用)'
    elif board == '科创板' and date < '2023-04-10':
        era = '纯2%拒单'
    elif board == '创业板' and date < '2020-08-24':
        era = '无笼时代'
    elif board == '创业板' and date < '2023-04-10':
        era = '暂存窗口(纯2%)'
    elif date < '2023-04-10':
        era = '无笼时代'
    else:
        era = '拒单时代(2%∨0.1)'
    return f'{board}·{era}'


def discover_all():
    """扫描 new_log/ 与 data/ 全部 (sym, date) 组合，自动判定板块与基金。"""
    pat = re.compile(r'csord_(\d{6})\.(SZ|SH)_(\d{4}-\d{2}-\d{2})\.csv')
    combos = set()
    for base in (NEW_LOG, DATA_DIR2):
        if not base.is_dir():
            continue
        for f in base.iterdir():
            m = pat.match(f.name)
            if m:
                code, mkt, date = m.groups()
                sym = f'{code}.{mkt}'
                combos.add((sym, date, code, mkt))
    out = []
    for sym, date, code, mkt in sorted(combos):
        is_etf = (mkt == 'SH' and code.startswith('5')) or \
                 (mkt == 'SZ' and code.startswith('1'))
        out.append((sym, date, _board_era_desc(code, mkt, date), is_etf))
    return out


def main():
    args = sys.argv[1:]
    use_all = '--all' in args
    want = {a for a in args if not a.startswith('--')}
    stock_list = discover_all() if use_all else STOCKS
    rows = []
    for sym, date, note, etf in stock_list:
        if want and sym not in want:
            continue
        try:
            rows.append(run_one(sym, date, note, etf))
        except Exception as e:  # 单只失败不阻断其余
            print(f"\n=== {sym} {date} [{note}] ===\n  失败: {e}")
            rows.append({"sym": sym, "date": date, "note": note, "err": str(e)})
    print("\n" + "=" * 88)
    print(f"{'股票':<12}{'日期':<12}{'板块':<24}{'子集匹配率':>10}{'假阴':>5}"
          f"{'多出':>5}{'快照对齐':>9}{'耗时':>6}")
    for r in rows:
        if "err" in r:
            print(f"{r['sym']:<12}{r['date']:<12}{r['note']:<24}{'ERROR':>10}")
            continue
        print(f"{r['sym']:<12}{r['date']:<12}{r['note']:<24}"
              f"{r['rate']*100:>9.2f}%{r['fn']:>5}{r['fp']:>5}"
              f"{r['align']*100:>8.2f}%{r['sec']:>5.0f}s")


if __name__ == "__main__":
    main()
