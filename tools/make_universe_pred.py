"""生成拒单时代前校验用的分层票池:板块 × 首日成交额降序抽样。

背景:此前全量校验票池按代码字符串排序取前 1000,全是深市主板(000/001/002),
既没有沪市票,也没有创业板/科创板/ETF。本脚本按板块分层配额抽样,保证
2022-06(主板无笼 + 创业板暂存 + 科创板拒单 + ETF 无笼 四种规则并存)的
规则矩阵都有足够样本。

必须用 adata 环境运行(账号从 ADATA_USER / ADATA_PASS 环境变量读取):

    /home/donghuale/venv_adata/bin/python tools/make_universe_pred.py \
        --day 2022-06-01 --out adata_universe_pred_450.txt

分层内按当日成交额(turnover)降序取样:活跃票价格笼子触发场景更多,
校验价值更高;巨型票由校验侧 --max-batch-rows 自动单独成批,不受影响。
"""

import argparse
import os
import sys
from pathlib import Path

import adata

# (层名, 判定函数, 配额)。ETF 前缀口径与 run_adata_validation.is_etf_sym 一致
# (15/16 深市,50/51/52/56/58 沪市);排除 511 货币式 ETF——价格钉在 100 元附近、
# 申赎套利成交模式极端,对撮合校验没有代表性。
LAYERS = [
    ("沪主板", lambda c, e: e == "SH" and c.startswith("60"), 110),
    ("深主板", lambda c, e: e == "SZ" and c.startswith("00"), 110),
    ("创业板", lambda c, e: e == "SZ" and c.startswith("30"), 120),
    ("科创板", lambda c, e: e == "SH" and c.startswith("68"), 80),
    ("ETF",    lambda c, e: c[:2] in {"15", "16", "50", "51", "52", "56", "58"}
                                 and not c.startswith("511"), 30),
]


def main():
    p = argparse.ArgumentParser(description="分层票池生成(拒单时代前校验用)")
    p.add_argument("--day", required=True, help="取样交易日 YYYY-MM-DD(时间窗首日)")
    p.add_argument("--out", required=True, help="输出票池文件(每行一个 sym)")
    args = p.parse_args()

    user = os.environ.get("ADATA_USER") or os.environ.get("ADATA_USERNAME")
    pw = os.environ.get("ADATA_PASS") or os.environ.get("ADATA_PASSWORD")
    if not user or not pw:
        sys.exit("缺少账号:请设置 ADATA_USER / ADATA_PASS 环境变量")
    adata.login(user, pw)

    bar = adata.get_data("csbar_1d", args.day, args.day, sym_list="*")
    if bar is None or len(bar) == 0:
        sys.exit(f"{args.day} 无全市场日线(非交易日?)")
    tcol = "turnover" if "turnover" in bar.columns else "amount"
    bar = bar[["sym", tcol]].dropna(subset=[tcol])
    bar[tcol] = bar[tcol].astype(float)

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

    print(f"📅 {args.day} 全市场 {len(bar):,} 只有行情,分层抽样 {len(picked)} 只:\n")
    for name, n, total, top in report:
        print(f"  {name:<4} {n:>3} 只  当层成交额合计 {total/1e8:>8.1f} 亿  头部: {top}")
    Path(args.out).write_text("\n".join(picked) + "\n")
    print(f"\n✅ 已写入 {args.out}")


if __name__ == "__main__":
    main()
