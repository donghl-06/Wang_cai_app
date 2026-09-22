#!/bin/bash
# 分层抽样回归波:四大战役各 25 只 × 1 交易周(500 只次全为历史 pass),
# 逐战役 拉→验→对比→删。用法: nohup bash tools/run_sample_regress.sh > logs/sample_regress.log 2>&1 &
set -uo pipefail
cd "$(dirname "$0")/.."
export TZ=Asia/Shanghai

FETCH_PY=/home/donghuale/venv_adata/bin/python
VENV_PY=.venv/bin/python

log() { echo "[$(date '+%m-%d %H:%M:%S')] $*"; }

run_camp() {  # $1=camp $2=start $3=end
    local camp=$1 d0=$2 d1=$3
    local dir=adata_sample_$camp exp=/tmp/sample_expected_$camp.csv
    local res=/tmp/sample_regress_$camp.csv
    log "===== $camp $d0~$d1 拉取 ====="
    rm -rf "$dir" "$res"
    $FETCH_PY fetch_adata_logs.py --start "$d0" --end "$d1" \
        --universe-file adata_universe_sample_$camp.txt --out "$dir" \
        > logs/sample_fetch_$camp.log 2>&1
    log "$camp 拉取完成:$(ls $dir | grep -c '^csord') 只次,开始校验"
    $VENV_PY tests/price_cage/run_adata_validation.py --data-dir "$dir" \
        --results "$res" --batch-size 12 > logs/sample_validate_$camp.log 2>&1
    log "$camp 校验完成,对比期望:"
    $VENV_PY - "$exp" "$res" <<'EOF'
import csv, sys
exp = {(r["sym"], r["date"]): r["status"]
       for r in csv.DictReader(open(sys.argv[1]))}
got = {}
for r in csv.DictReader(open(sys.argv[2])):
    got[(r["sym"], r["date"])] = (r["status"], r["false_neg"], r["false_pos"])
flips, same, miss = [], 0, []
for k, v in exp.items():
    if k not in got:
        miss.append(k)
    elif got[k][0] != v:
        flips.append((k, v, got[k]))
    else:
        same += 1
print(f"  一致 {same}/{len(exp)},翻转 {len(flips)},缺记录 {len(miss)}")
for k, v, g in flips:
    print(f"  [翻转] {k[0]} {k[1]} 期望 {v} -> 实际 {g[0]} fn={g[1]} fp={g[2]}")
for k in miss[:10]:
    print(f"  [缺记录] {k[0]} {k[1]}")
EOF
    log "$camp 完成,删除原始数据"
    rm -rf "$dir"
}

run_camp g20  2020-09-07 2020-09-11
run_camp n21  2021-07-05 2021-07-09
run_camp pred 2022-06-27 2022-07-01
run_camp reg  2023-04-10 2023-04-14
log "===== 全部抽样波完成 ====="
