"""adata 全量数据成交一致性验证:价格笼子重建成交 vs 官方 cstra 逐笔对账。

用法(项目根目录,用项目 .venv 跑):
    .venv/bin/python tests/price_cage/run_adata_validation.py                 # 全量
    .venv/bin/python tests/price_cage/run_adata_validation.py --limit 200     # 只跑前 200 只次
    .venv/bin/python tests/price_cage/run_adata_validation.py --batch-size 8 --date 2024-11-01

口径(与 run_real_stocks.py 一致):
- 连续竞价段 09:30–14:57、ExecType=1 的成交,按 (channelno, bidorderid, askorderid, price)
  聚合成交量,引擎 vs 官方 cstra 要求**完全一致**(子集匹配率 100% + 假阴 0 + 多出 0)。
- 本脚本不评估快照对齐率(参考指标,见 docs/REAL_DATA_FULL_VALIDATION.md)。

并行模型:
- 每次 run_backtest 送入 --batch-size 只同日期合约:CSV 转换走 n_workers 多进程,
  引擎内部按时间戳归并、同一时间戳的不同合约由 Taskflow 多线程并行撮合
  (见 multi_backtest_engine.cpp run())。
- 批次间串行;4 核机器建议 batch-size 4~8(内存 15GB,单批不宜过大)。

进度与续传:
- 每批结束即打印进度/累计一致率/ETA,并把该批结果**追加写入** --results CSV,
  中断后重跑自动跳过已完成只次(--retry-failed 可重跑失败只次)。
"""

import argparse
import csv
import os
import re
import sys
import time
from collections import defaultdict
from pathlib import Path

import pandas as pd

from wangcai_syn import Strategy, run_backtest
from wangcai_syn.utils import _normalize_table_layout

ROOT = Path(__file__).resolve().parent.parent.parent

RESULT_FIELDS = ["sym", "date", "status", "n_real_pairs", "n_eng_pairs",
                 "subset_rate", "false_neg", "false_pos", "exact", "seconds", "note"]


def parse_args():
    p = argparse.ArgumentParser(description="adata 全量成交一致性验证")
    p.add_argument("--data-dir", default=str(ROOT / "adata_logs"), help="四件套 CSV 目录")
    p.add_argument("--results", default=str(ROOT / "adata_validation_results.csv"),
                   help="结果 CSV(增量追加,用于续传)")
    p.add_argument("--batch-size", type=int, default=6, help="单次 run_backtest 的合约数")
    p.add_argument("--limit", type=int, default=0, help="最多跑多少只次(0=全部)")
    p.add_argument("--date", default=None, help="只跑指定日期 YYYY-MM-DD")
    p.add_argument("--retry-failed", action="store_true",
                   help="重跑结果 CSV 中非 pass 的只次(默认跳过所有已记录只次)")
    return p.parse_args()


# ---------- 数据加载(口径沿用 run_real_stocks.load_files) ----------

def is_etf_sym(sym: str) -> bool:
    code = sym.split(".")[0]
    return code.startswith(("15", "16", "50", "51", "52", "56", "58"))


def load_one(base: Path, sym: str, day: str):
    files = {}
    for k in ("cstick", "csord", "cstra"):
        fp = base / f"{k}_{sym}_{day}.csv"
        if not fp.is_file():
            raise FileNotFoundError(fp.name)
        files[k] = pd.read_csv(fp)
    if len(files["cstick"]) == 0:
        raise ValueError("cstick 为空")
    bar = base / f"csbar1d_{sym}_{day}.csv"
    if bar.is_file() and bar.stat().st_size > 0:
        bdf = pd.read_csv(bar)
    else:
        bdf = pd.DataFrame()
    if len(bdf) == 0:
        # 缺日线:用 cstick prevclose 合成(同 run_real_stocks 的兜底)
        prev = float(files["cstick"]["prevclose"].dropna().iloc[-1])
        bdf = pd.DataFrame([{
            "sym": sym, "prevclose": prev, "open": prev, "high": prev,
            "low": prev, "close": prev, "volume": 0, "turnover": 0,
            "tradecount": 0, "af": 1.0,
            "upperlimit": prev * 1.2, "lowerlimit": prev * 0.8}])
    files["csbar1d"] = bdf
    for k in ("cstick", "csord", "cstra"):
        files[k] = _normalize_table_layout(files[k], k)
    return files


# ---------- 成交收集与对账(口径沿用 run_real_stocks) ----------

class Collector(Strategy):
    """多合约版:按 Instrument 分桶收集连续段重建成交(ExecType=1)

    性能要点:
    - 用 onTradeEventsBatch 批量接收(每市场事件一次跨界,而非每笔成交一次)
    - 不覆写 onOrderEvent/onTickEvent 等无关回调,交给 C++ 覆写探测缓存短路
    """

    def __init__(self):
        super().__init__()
        self.trades = defaultdict(list)  # sym -> [(ch, buy, sell, px, vol)]

    def getStrategyId(self):
        return "RW"

    def onTradeEventsBatch(self, ts):
        for t in ts:
            if t.ExecType == '1':
                hhmmss = t.Time // 1000
                if 93000 <= hhmmss < 145700:
                    self.trades[t.Instrument].append(
                        (int(t.ChannelNo), int(t.BuyNo), int(t.SellNo),
                         int(t.Price), int(t.Volume)))
        return []


def _norm_bytes(v):
    t = str(v)
    return t[2:-1] if t.startswith("b'") else t.strip()


def real_agg(cstra):
    ts = cstra["time"].astype(str)
    ex = cstra["exectype"].map(_norm_bytes)
    df = cstra[(ex == "1") & (ts >= "0 days 09:30:00") & (ts < "0 days 14:57:00")]
    agg = {}
    for r in df.itertuples(index=False):
        k = (int(r.channelno), int(r.bidorderid), int(r.askorderid),
             int(round(float(r.price) * 10000)))
        agg[k] = agg.get(k, 0) + int(r.size)
    return agg


def eng_agg(trade_list):
    agg = {}
    for ch, b, a, p, v in trade_list:
        k = (ch, b, a, p)
        agg[k] = agg.get(k, 0) + v
    return agg


# ---------- 结果 CSV(增量追加 + 续传) ----------

def load_done(results_path: Path, retry_failed: bool):
    done = set()
    if results_path.is_file():
        with open(results_path, newline="") as f:
            for row in csv.DictReader(f):
                if retry_failed and row["status"] != "pass":
                    continue
                done.add((row["sym"], row["date"]))
    return done


def append_results(results_path: Path, rows):
    write_header = not results_path.is_file()
    with open(results_path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=RESULT_FIELDS)
        if write_header:
            w.writeheader()
        w.writerows(rows)
        f.flush()


# ---------- 主流程 ----------

def main():
    args = parse_args()
    base = Path(args.data_dir)
    results_path = Path(args.results)

    # 扫描全部 (sym, date)
    pat = re.compile(r"^cstra_(.+\.(?:SH|SZ))_(\d{4}-\d{2}-\d{2})\.csv$")
    pairs = sorted(
        (m.group(1), m.group(2))
        for f in base.glob("cstra_*.csv")
        if (m := pat.match(f.name)))
    if args.date:
        pairs = [(s, d) for s, d in pairs if d == args.date]
    done = load_done(results_path, args.retry_failed)
    pairs = [(s, d) for s, d in pairs if (s, d) not in done]
    if args.limit > 0:
        pairs = pairs[:args.limit]
    total = len(pairs)
    print(f"📋 待验证 {total} 只次(跳过已完成 {len(done)} 只次),"
          f"batch-size={args.batch_size}", flush=True)
    if total == 0:
        return

    # 按日期分组,日内按 batch-size 切批
    by_date = defaultdict(list)
    for s, d in pairs:
        by_date[d].append(s)
    batches = []
    for d in sorted(by_date):
        syms = sorted(by_date[d])
        batches.extend((d, syms[i:i + args.batch_size])
                       for i in range(0, len(syms), args.batch_size))

    n_pairs_done = n_pass = 0
    t_start = time.time()

    for bi, (day, syms) in enumerate(batches, 1):
        t_batch = time.time()
        data_dict, reals, rows = {}, {}, []
        for sym in syms:
            try:
                files = load_one(base, sym, day)
            except Exception as e:
                rows.append({"sym": sym, "date": day, "status": "load_error",
                             "n_real_pairs": 0, "n_eng_pairs": 0, "subset_rate": 0,
                             "false_neg": 0, "false_pos": 0, "exact": 0,
                             "seconds": 0, "note": str(e)[:80]})
                continue
            data_dict[sym] = (files["cstick"], files["csord"], files["cstra"],
                              files["csbar1d"], is_etf_sym(sym))
            reals[sym] = real_agg(files["cstra"])  # release_input 前先算好真值

        if data_dict:
            collector = Collector()
            batch_syms = list(data_dict)  # release_input 会清空字典,先存名单
            ok = False
            try:
                ok = run_backtest(data_dict, collector,
                                  n_workers=min(len(data_dict), os.cpu_count()),
                                  release_input=True)
            except Exception as e:
                for sym in batch_syms:
                    rows.append({"sym": sym, "date": day, "status": "engine_error",
                                 "n_real_pairs": 0, "n_eng_pairs": 0, "subset_rate": 0,
                                 "false_neg": 0, "false_pos": 0, "exact": 0,
                                 "seconds": 0, "note": str(e)[:80]})
            if not ok and not any(r["sym"] in batch_syms for r in rows):
                # run_backtest 内部消化了异常并返回 False:整批记 engine_fail
                for sym in batch_syms:
                    rows.append({"sym": sym, "date": day, "status": "engine_fail",
                                 "n_real_pairs": len(reals[sym]), "n_eng_pairs": 0,
                                 "subset_rate": 0, "false_neg": 0, "false_pos": 0,
                                 "exact": 0, "seconds": 0, "note": "run_backtest 返回 False"})
            if ok:
                unknown = set(collector.trades) - set(batch_syms)
                if unknown:
                    print(f"   ⚠️ 引擎回报了批次外合约 {unknown},请检查 Instrument 字段!",
                          flush=True)
                for sym in batch_syms:
                    eng, real = eng_agg(collector.trades.get(sym, [])), reals[sym]
                    matched = sum(1 for k, v in real.items() if eng.get(k, 0) >= v)
                    fn = sum(1 for k, v in real.items() if eng.get(k, 0) < v)
                    fp = sum(1 for k in eng if k not in real)
                    exact = int(eng == real)
                    rows.append({
                        "sym": sym, "date": day,
                        "status": "pass" if exact else "fail",
                        "n_real_pairs": len(real), "n_eng_pairs": len(eng),
                        "subset_rate": round(matched / max(len(real), 1), 6),
                        "false_neg": fn, "false_pos": fp, "exact": exact,
                        "seconds": 0, "note": ""})
                    if not exact:
                        miss = [(k, v, eng.get(k, 0)) for k, v in real.items()
                                if eng.get(k, 0) < v][:3]
                        extra = [(k, v) for k, v in eng.items() if k not in real][:3]
                        print(f"   ❌ {sym} {day}: 假阴{fn} 多出{fp} "
                              f"缺样例{miss} 多样例{extra}", flush=True)

        append_results(results_path, rows)
        n_pairs_done += len(rows)
        n_pass += sum(1 for r in rows if r["status"] == "pass")
        el = time.time() - t_start
        done_ratio = n_pairs_done / max(total, 1)
        eta = el / done_ratio * (1 - done_ratio) if n_pairs_done else 0
        print(f"[批 {bi}/{len(batches)}] {day} ×{len(syms)} 只 "
              f"本批 {time.time()-t_batch:.0f}s | 累计 {n_pairs_done}/{total} 只次 "
              f"一致率 {n_pass/max(n_pairs_done,1)*100:.2f}% "
              f"({n_pass} pass / {n_pairs_done-n_pass} 非pass) | "
              f"已跑 {el/60:.1f}min ETA {eta/60:.1f}min", flush=True)

    print(f"\n{'='*60}\n最终结果:{n_pass}/{n_pairs_done} 只次完全一致 "
          f"({n_pass/max(n_pairs_done,1)*100:.2f}%)\n结果文件:{results_path}\n{'='*60}")
    sys.stdout.flush()
    os._exit(0)  # 绕过 C++ 引擎析构卡死(见 benchmark_results.md 关键发现 5)


if __name__ == "__main__":
    main()
