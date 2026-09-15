"""adata 批量数据拉取:A股全市场(或指定数量)× 日期范围 → adata_logs/ 四件套 CSV

必须用 adata 环境运行(adata 装在独立 venv,项目 .venv 里没有):

    /home/donghuale/venv_adata/bin/python fetch_adata_logs.py \
        --start 2024-10-01 --end 2024-12-31 --universe-size 1000

说明:
- 账号从环境变量读取:ADATA_USER / ADATA_PASS(兼容 ADATA_USERNAME / ADATA_PASSWORD)。
- 数据权限范围 2020-01-01 ~ 2024-12-31(订阅口径,超出会报 PermissionError)。
- 股票池:首个交易日 csbar_1d(sym_list='*')全市场快照中,按代码前缀过滤 A 股
  (SH: 60xxxx/68xxxx,SZ: 00xxxx/30xxxx;剔除指数/ETF/BJ),排序后取前 N 只。
- 每个日期:1 次 csbar_1d 全市场调用(兼作交易日探测 + 日线数据源)
  + cstick/csord/cstra 三张表按 --fetch-chunk 只一批调用,按 sym 拆写 CSV。
- 输出命名与 new_log/data 一致: {table}_{sym}_{date}.csv(csbar_1d → csbar1d)。
- 断点续传:默认跳过四件套已齐的 (sym, date);停牌只次天然缺失,重跑会重试。
"""

import argparse
import os
import sys
import time
from datetime import date as date_cls, timedelta
from pathlib import Path

import adata

OUT_DIR = Path("./adata_logs")
TABLES = ("cstick", "csord", "cstra")  # csbar1d 来自全市场日线,不单独拉


def parse_args():
    p = argparse.ArgumentParser(description="adata A股日志数据批量导出")
    p.add_argument("--start", required=True, help="起始日期 YYYY-MM-DD")
    p.add_argument("--end", required=True, help="结束日期 YYYY-MM-DD")
    p.add_argument("--universe-size", type=int, default=1000, help="股票池大小(0=全部A股)")
    p.add_argument("--offset", type=int, default=0, help="股票池起始偏移(分波用)")
    p.add_argument("--limit", type=int, default=0, help="本波最多拉多少只(0=不限)")
    p.add_argument("--universe-file", default="",
                   help="股票池冻结文件:存在则读取,不存在则首个交易日计算后写入。"
                        "分波拉取必须用它保证各波股票池一致(每日快照成分会变)")
    p.add_argument("--fetch-chunk", type=int, default=50, help="每次 get_data 调用的股票数")
    p.add_argument("--out", default=str(OUT_DIR), help="输出目录")
    p.add_argument("--no-skip", action="store_true", help="不跳过已存在文件,全部重拉")
    p.add_argument("--retry", type=int, default=3, help="单批失败重试次数")
    return p.parse_args()


def login():
    user = os.environ.get("ADATA_USER") or os.environ.get("ADATA_USERNAME")
    pw = os.environ.get("ADATA_PASS") or os.environ.get("ADATA_PASSWORD")
    if not user or not pw:
        sys.exit("缺少账号:请设置 ADATA_USER / ADATA_PASS 环境变量")
    adata.login(user, pw)


def is_a_share(sym: str) -> bool:
    code, _, exch = sym.partition(".")
    if exch == "SH":
        return code.startswith(("60", "68"))
    if exch == "SZ":
        return code.startswith(("00", "30"))
    return False


def get_with_retry(table, day, syms, retry):
    for attempt in range(1, retry + 1):
        try:
            return adata.get_data(table, day, day, syms)
        except Exception as e:
            if attempt == retry:
                raise
            wait = 2 * attempt
            print(f"      ⚠️ {table} 第{attempt}次失败({e}),{wait}s 后重试...", flush=True)
            time.sleep(wait)


def four_files_ready(out: Path, sym: str, day: str) -> bool:
    return all((out / f"{t}_{sym}_{day}.csv").is_file()
               for t in ("cstick", "csord", "cstra", "csbar1d"))


def main():
    args = parse_args()
    login()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    d0 = date_cls.fromisoformat(args.start)
    d1 = date_cls.fromisoformat(args.end)
    days = [(d0 + timedelta(days=i)).isoformat()
            for i in range((d1 - d0).days + 1)]

    universe = None
    n_done_days = 0
    t_start = time.time()

    for di, day in enumerate(days, 1):
        # ---- 全市场日线:兼作交易日探测与股票池来源 ----
        try:
            bar_all = adata.get_data("csbar_1d", day, day, sym_list="*")
        except Exception as e:
            print(f"[{di}/{len(days)}] {day} 日线拉取失败,跳过({e})", flush=True)
            continue
        if bar_all is None or len(bar_all) == 0:
            print(f"[{di}/{len(days)}] {day} 非交易日,跳过", flush=True)
            continue

        if universe is None:
            if args.universe_file and Path(args.universe_file).is_file():
                universe = [s.strip() for s in open(args.universe_file) if s.strip()]
                print(f"📋 股票池:{len(universe)} 只(冻结文件 {args.universe_file})", flush=True)
            else:
                pool = sorted(s for s in bar_all["sym"].unique() if is_a_share(s))
                if args.universe_size > 0:
                    pool = pool[:args.universe_size]
                universe = pool
                print(f"📋 股票池:{len(universe)} 只 A 股(首个交易日 {day} 确定)", flush=True)
                if args.universe_file:
                    Path(args.universe_file).write_text("\n".join(universe) + "\n")
                    print(f"📋 股票池已冻结到 {args.universe_file}", flush=True)
            # 本波切片
            if args.offset or args.limit:
                universe = universe[args.offset: args.offset + args.limit or None]
                print(f"📋 本波切片:offset={args.offset} 共 {len(universe)} 只", flush=True)

        day_syms = set(bar_all["sym"])  # 当日有日线的票(剔除长期停牌)
        pending = [s for s in universe if s in day_syms]
        if not args.no_skip:
            pending = [s for s in pending if not four_files_ready(out, s, day)]
        n_done_days += 1

        if not pending:
            print(f"[{di}/{len(days)}] {day} 全部已存在,跳过", flush=True)
            continue

        print(f"[{di}/{len(days)}] {day} 待拉取 {len(pending)} 只...", flush=True)
        t_day = time.time()

        # ---- csbar1d:直接从全市场日线切,零额外调用 ----
        bar_day = bar_all[bar_all["sym"].isin(pending)]
        for sym, g in bar_day.groupby("sym"):
            g.to_csv(out / f"csbar1d_{sym}_{day}.csv", index=False)

        # ---- 三张逐笔表:分批拉取,按 sym 拆写 ----
        for table in TABLES:
            chunks = [pending[i:i + args.fetch_chunk]
                      for i in range(0, len(pending), args.fetch_chunk)]
            n_rows = 0
            for ci, chunk in enumerate(chunks, 1):
                try:
                    df = get_with_retry(table, day, chunk, args.retry)
                except Exception as e:
                    print(f"      ❌ {table} 第{ci}/{len(chunks)}批最终失败:{e}", flush=True)
                    continue
                if df is None or len(df) == 0:
                    continue
                n_rows += len(df)
                for sym, g in df.groupby("sym"):
                    g.to_csv(out / f"{table}_{sym}_{day}.csv", index=False)
                if ci % 4 == 0 or ci == len(chunks):
                    print(f"      {table}: {ci}/{len(chunks)} 批,累计 {n_rows:,} 行",
                          flush=True)

        el = time.time() - t_start
        eta = el / n_done_days * (len(days) - di)
        print(f"   ✅ {day} 完成,当日 {time.time()-t_day:.0f}s;"
              f"总耗时 {el/60:.1f}min,预计剩余 {eta/60:.1f}min", flush=True)

    print(f"\n全部完成,总耗时 {(time.time()-t_start)/60:.1f} 分钟")


if __name__ == "__main__":
    main()
