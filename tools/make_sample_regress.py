"""分层抽样回归:从四大战役各挑一个交易周 × 25 只分层股票,生成
- adata_universe_sample_<camp>.txt  冻结股票池(供 fetch_adata_logs.py --universe-file)
- /tmp/sample_expected_<camp>.csv   网格内只次的历史状态(sym,date,status,last-wins)

抽样周(避开新股首日密集周,reg 特意选全面注册制首周):
  g20  2020-09-07~09-11   n21  2021-07-05~07-09
  pred 2022-06-27~07-01   reg  2023-04-10~04-14
"""
import csv
from collections import defaultdict
from pathlib import Path

CAMPS = {
    "g20": ("adata_universe_g20_620.txt", "adata_validation_results_g20.csv",
            ["2020-09-07", "2020-09-08", "2020-09-09", "2020-09-10", "2020-09-11"]),
    "n21": ("adata_universe_n21_620.txt", "adata_validation_results_n21.csv",
            ["2021-07-05", "2021-07-06", "2021-07-07", "2021-07-08", "2021-07-09"]),
    "pred": ("adata_universe_pred_450.txt", "adata_validation_results_pred.csv",
             ["2022-06-27", "2022-06-28", "2022-06-29", "2022-06-30", "2022-07-01"]),
    "reg": ("adata_universe_reg_600.txt", "adata_validation_results_reg.csv",
            ["2023-04-10", "2023-04-11", "2023-04-12", "2023-04-13", "2023-04-14"]),
}
N_SYMS = 25


def board(sym: str) -> str:
    code = sym.split(".")[0]
    for p, b in (("68", "STAR"), ("60", "SH"), ("30", "GEM"), ("00", "SZ")):
        if code.startswith(p):
            return b
    return "OTHER"


def last_wins(path: str) -> dict:
    st = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            st[(r["sym"], r["date"])] = r["status"]
    return st


def main():
    for camp, (uni_file, csv_file, days) in CAMPS.items():
        uni = [s.strip() for s in open(uni_file) if s.strip()]
        st = last_wins(csv_file)
        # 按板分组,等距抽样;优先该周 pass 覆盖高的股票
        by_board = defaultdict(list)
        for s in uni:
            by_board[board(s)].append(s)
        quota = {}
        rem = N_SYMS
        boards = [b for b in ("SH", "SZ", "GEM", "STAR") if by_board[b]]
        for b in boards[:-1]:
            quota[b] = max(3, round(N_SYMS * len(by_board[b]) / len(uni)))
            rem -= quota[b]
        quota[boards[-1]] = rem
        picked = []
        for b in boards:
            pool = sorted(by_board[b],
                          key=lambda s: -sum(st.get((s, d)) == "pass" for d in days))
            k = quota[b]
            step = max(1, len(pool) // k)
            picked += pool[::step][:k]
        picked = sorted(set(picked))
        Path(f"adata_universe_sample_{camp}.txt").write_text(
            "\n".join(picked) + "\n")
        rows = [(s, d, st.get((s, d), "missing")) for s in picked for d in days]
        with open(f"/tmp/sample_expected_{camp}.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["sym", "date", "status"])
            w.writerows(rows)
        from collections import Counter
        print(f"{camp}: {len(picked)} 只 {[board(s) for s in picked].count('SH')}SH/"
          f"{[board(s) for s in picked].count('SZ')}SZ/"
          f"{[board(s) for s in picked].count('GEM')}GEM/"
          f"{[board(s) for s in picked].count('STAR')}STAR, "
          f"网格状态 {dict(Counter(r[2] for r in rows))}")


if __name__ == "__main__":
    main()
