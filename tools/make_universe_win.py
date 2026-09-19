"""生成指定窗口校验用的分层票池:板块 × 首日成交额降序抽样 + 新股首日层。

仿 make_universe_reg.py,通用化为任意窗口,并新增"新股首日层":
逐日探测窗口内全市场日线,首见日落在窗口内且基准日(窗口前约 45 天)
不存在的票判为窗口内新上市,按其上市日成交额降序取 --ipo-quota 只。
新股首日无涨跌幅,专门压测无涨跌幅簿边界封顶 + 簿外价吸收路径
(301408.SZ 2023-03-01 修复, commit 92da993)。

用法(必须用 adata 环境,账号从 ADATA_USER / ADATA_PASS 环境变量读取):

    /home/donghuale/venv_adata/bin/python tools/make_universe_win.py \
        --start 2020-08-03 --end 2020-09-11 --out adata_universe_g20_620.txt

注意:adata 按当前代码追溯返回历史数据,2025 年后启用的新代码段
(301/302 等)可能映射进老窗口(302132 污染实证);命中的废票在结果
排查阶段登记例外,不在抽样期拦截。
"""

import argparse
import datetime as dt
import os
import sys
import time
from pathlib import Path

import adata

# (层名, 判定函数, 配额)。口径与 make_universe_reg.py 一致;排除 511 货币式
# ETF(价格钉在 100 元附近、申赎套利成交模式极端,对撮合校验没有代表性)。
LAYERS = [
    ("沪主板", lambda c, e: e == "SH" and c.startswith("60"), 140),
    ("深主板", lambda c, e: e == "SZ" and c.startswith("00"), 140),
    ("创业板", lambda c, e: e == "SZ" and c.startswith("30"), 150),
    ("科创板", lambda c, e: e == "SH" and c.startswith("68"), 100),
    ("ETF",    lambda c, e: c[:2] in {"15", "16", "50", "51", "52", "56", "58"}
                                 and not c.startswith("511"), 70),
]

# 新股层只收股票板块(60/00/30/68),新上市基金/转债不算新股首日场景
STOCK_PREFIX = ("60", "00", "30", "68")


def fetch_bar(day: str):
    """拉全市场日线,非交易日返回 None"""
    bar = adata.get_data("csbar_1d", day, day, sym_list="*")
    if bar is None or len(bar) == 0:
        return None
    return bar


def find_prev_trading_day(start: str, back_days: int) -> tuple:
    """从 start 往前 back_days 个日历天起,找最近交易日的 (日期, 全市场日线)。
    back_days=1 → 窗口前一交易日(新股判定:在场即非窗口内新上市);
    back_days=45 → 基准日(双过滤:拦截 adata 代码追溯污染,302132 实证)。"""
    d = dt.date.fromisoformat(start) - dt.timedelta(days=back_days)
    for _ in range(15):
        bar = fetch_bar(d.isoformat())
        if bar is not None:
            return d.isoformat(), bar
        d -= dt.timedelta(days=1)
    sys.exit(f"前向交易日探测失败(start={start}, back={back_days})")


def main():
    p = argparse.ArgumentParser(description="分层票池生成(任意窗口 + 新股首日层)")
    p.add_argument("--start", required=True, help="窗口首日 YYYY-MM-DD")
    p.add_argument("--end", required=True, help="窗口末日 YYYY-MM-DD")
    p.add_argument("--out", required=True, help="输出票池文件(每行一个 sym)")
    p.add_argument("--ipo-quota", type=int, default=20, help="新股首日层配额")
    args = p.parse_args()

    user = os.environ.get("ADATA_USER") or os.environ.get("ADATA_USERNAME")
    pw = os.environ.get("ADATA_PASS") or os.environ.get("ADATA_PASSWORD")
    if not user or not pw:
        sys.exit("缺少账号:请设置 ADATA_USER / ADATA_PASS 环境变量")
    adata.login(user, pw)

    # ---- 逐日探测窗口内全市场 sym 集合(同时攒上市日成交额) ----
    tcol = None
    seen = {}          # sym -> 首见日
    turnover = {}      # sym -> 首见日成交额
    day = dt.date.fromisoformat(args.start)
    end = dt.date.fromisoformat(args.end)
    first_bar = None
    n_probe = 0
    while day <= end:
        bar = fetch_bar(day.isoformat())
        if bar is None:
            day += dt.timedelta(days=1)
            continue
        n_probe += 1
        if tcol is None:
            tcol = "turnover" if "turnover" in bar.columns else "amount"
        if first_bar is None:
            first_bar = bar
        tv = dict(zip(bar["sym"], bar[tcol].astype(float)))
        for sym in bar["sym"]:
            if sym not in seen:
                seen[sym] = day.isoformat()
                turnover[sym] = tv.get(sym, 0.0)
        day += dt.timedelta(days=1)
        time.sleep(0.2)  # 温和打接口
    if first_bar is None:
        sys.exit(f"{args.start}~{args.end} 无交易日")

    # ---- 新股首日层:首见日在窗口内,且窗口前一交易日与基准日都不在场 ----
    # (双重过滤:前者精确排除窗口前已上市,后者拦截代码追溯污染的票——
    #  adata 按当前代码追溯返回历史,改名票在两个前向探测日均在场)
    prev_day, prev_bar = find_prev_trading_day(args.start, 1)
    baseline_day, baseline_bar = find_prev_trading_day(args.start, 45)
    prev_syms = set(prev_bar["sym"])
    baseline_syms = set(baseline_bar["sym"])
    ipo_pool = [s for s, d0 in seen.items()
                if s not in prev_syms and s not in baseline_syms
                and s.partition(".")[0].startswith(STOCK_PREFIX)]
    ipo_pool.sort(key=lambda s: turnover.get(s, 0.0), reverse=True)

    # ---- 5 层板块抽样(取样日=窗口首个交易日,口径与 reg/pred 一致) ----
    bar = first_bar[["sym", tcol]].dropna(subset=[tcol])
    bar = bar[bar[tcol].astype(float).notna()]

    picked, report = [], []
    remaining = set(bar["sym"])
    for name, match, quota in LAYERS:
        def hit(s):
            code, _, exch = s.partition(".")
            return match(code, exch)
        pool = bar[bar["sym"].isin({s for s in remaining if hit(s)})]
        pool = pool.sort_values(tcol, ascending=False)
        take = pool.head(quota)
        if len(take) < quota:
            print(f"⚠️ {name}:符合 {len(take)} 只 < 配额 {quota}", file=sys.stderr)
        syms = take["sym"].tolist()
        picked += syms
        remaining -= set(syms)
        top3 = take.head(3)
        top = ", ".join(f"{s}({v/1e8:.1f}亿)"
                        for s, v in zip(top3["sym"], top3[tcol]))
        report.append((name, len(syms), take[tcol].sum(), top))

    # 新股层(剔除已被 5 层命中的)
    ipo_take = [s for s in ipo_pool if s not in set(picked)][:args.ipo_quota]
    picked += ipo_take

    print(f"📅 {args.start}~{args.end} 探测 {n_probe} 个交易日,"
          f"基准日 {baseline_day};分层抽样 {len(picked)} 只:\n")
    for name, n, total, top in report:
        print(f"  {name:<4} {n:>3} 只  当层成交额合计 {total/1e8:>8.1f} 亿  头部: {top}")
    print(f"  新股   {len(ipo_take):>3} 只  "
          f"(窗口内新上市共 {len(ipo_pool)} 只,取成交额前 {args.ipo_quota})")
    for s in ipo_take[:5]:
        print(f"    {s} 上市日 {seen[s]} 首日成交 {turnover.get(s, 0)/1e8:.1f} 亿")
    Path(args.out).write_text("\n".join(picked) + "\n")
    print(f"\n✅ 已写入 {args.out} ({len(picked)} 只)")


if __name__ == "__main__":
    main()
