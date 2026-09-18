#!/usr/bin/env python3
"""清理块150 保留数据:只留失败只次/保护对的四件套,删除已 pass 只次。

用法:
  python3 tools/cleanup_block150.py          # dry-run,只打印统计
  python3 tools/cleanup_block150.py --go     # 真删
"""
import csv
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 1. 失败只次(2022-06/07 块150 窗口) = 需要保留证据的
fails = set()
for r in csv.DictReader(open(f"{ROOT}/adata_validation_results_pred.csv")):
    if r["status"] not in ("pass",) and r["date"].startswith("2022-"):
        fails.add((r["sym"], r["date"]))

# 2. 回归基准保护对(与 run_waves_pred.sh 删除逻辑同口径)
protect = set()
pp = f"{ROOT}/tools/protect_pairs.txt"
if os.path.exists(pp):
    for line in open(pp):
        line = line.strip()
        if line:
            protect.add(line)

keep = fails | {tuple(p.rsplit("_", 1)) for p in protect
                if "_" in p and p.rsplit("_", 1)[1].startswith("2022-")}

n_keep = n_del = 0
sz_del = 0
to_del = []
for f in glob.glob(f"{ROOT}/adata_logs/*.csv"):
    base = os.path.basename(f)[:-4]
    _, rest = base.split("_", 1)          # 去掉 cstra_/csord_/... 前缀
    sym, date = rest.rsplit("_", 1)
    if not ("2022-06-01" <= date <= "2022-07-05"):
        continue                           # 只动本次窗口;2024 残留另处理
    if (sym, date) in keep:
        n_keep += 1
    else:
        to_del.append(f)
        n_del += 1
        sz_del += os.path.getsize(f)

print(f"窗口内文件: 保留 {n_keep}(失败 {len(fails)} 只次四件套 + 保护对), "
      f"待删 {n_del} 个文件, 可释放 {sz_del/1e9:.1f}G")
if "--go" in sys.argv:
    for f in to_del:
        os.remove(f)
    print(f"已删除 {len(to_del)} 个文件")
else:
    print("dry-run 未删除;确认后加 --go 执行")
