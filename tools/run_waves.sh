#!/bin/bash
# 分波驱动:股票块(200只) × 周日历块(≈5 个交易日) 双层分波
#   每子波:拉取 → 校验 → 全过则删原始数据 → 提交结果 → 下一子波
# 用法: nohup bash tools/run_waves.sh > logs/waves.log 2>&1 &
# 进度: tail -f logs/waves.log(明细 logs/fetch.log、logs/validate.log)
# 结果: adata_validation_results.csv(增量,断点续传,逐子波 git 提交)
set -uo pipefail
cd "$(dirname "$0")/.."

FETCH_PY=/home/donghuale/venv_adata/bin/python
VENV_PY=.venv/bin/python
START=2024-10-08
END=2024-12-31
UNIVERSE=1000
UNIVERSE_FILE=adata_universe_1000.txt
BLOCK_SIZE=200         # 股票块大小(5 块 × 200 = 1000)
CHUNK_DAYS=7           # 日历天/子波(≈5 个交易日)
RESULTS=adata_validation_results.csv

log() { echo "[$(date '+%m-%d %H:%M:%S')] $*"; }

for (( offset=0; offset<UNIVERSE; offset+=BLOCK_SIZE )); do
    chunk_start=$START
    while [[ "$chunk_start" < "$END" ]]; do
        chunk_end=$(date -d "$chunk_start +$((CHUNK_DAYS-1)) days" +%F)
        [[ "$chunk_end" > "$END" ]] && chunk_end=$END
        tag="块$((offset))-$((offset+BLOCK_SIZE-1)) @ $chunk_start~$chunk_end"
        log "===== 子波 $tag ====="

        # 1. 拉取(断点续传:已齐四件套的只次自动跳过)
        log "拉取中(磁盘余量 $(df -h . | awk 'NR==2{print $4}'))..."
        if ! $FETCH_PY fetch_adata_logs.py --start "$chunk_start" --end "$chunk_end" \
                --universe-size $UNIVERSE --offset $offset --limit $BLOCK_SIZE \
                --universe-file "$UNIVERSE_FILE" >> logs/fetch.log 2>&1; then
            log "❌ 拉取失败,见 logs/fetch.log,60s 后重试本子波"
            sleep 60; continue
        fi
        log "拉取完成(磁盘余量 $(df -h . | awk 'NR==2{print $4}')),开始校验"

        # 2. 校验(增量:已 pass 的只次自动跳过)
        if ! $VENV_PY tests/price_cage/run_adata_validation.py >> logs/validate.log 2>&1; then
            log "❌ 校验进程异常退出,见 logs/validate.log;保留数据,60s 后重试"
            sleep 60; continue
        fi

        # 3. 本子波只次状态统计(同一 (sym,date) 取最后一次记录;
        #    data_incomplete=源数据缺行,已如实登记,视为已处置)
        wave_stat=$($VENV_PY - "$chunk_start" "$chunk_end" "$RESULTS" <<'EOF'
import csv, sys
s, e = sys.argv[1], sys.argv[2]
best = {}
for r in csv.DictReader(open(sys.argv[3])):
    if s <= r["date"] <= e:
        best[(r["sym"], r["date"])] = r["status"]
n = len(best)
p = sum(1 for v in best.values() if v == "pass")
inc = sum(1 for v in best.values() if v == "data_incomplete")
print(f"{p} {inc} {n}")
EOF
)
        read -r n_pass n_inc n_done <<< "$wave_stat"
        log "本子波结果:$n_pass pass + $n_inc data_incomplete / 共 $n_done"

        # 4. 全部 pass/data_incomplete 才删本子波原始数据(其余保留待查;
        #    跳过 tools/protect_pairs.txt 里的回归基准只次)
        if [[ "$n_done" -gt 0 && $((n_pass + n_inc)) == "$n_done" ]]; then
            d=$chunk_start
            while [[ "$d" < "$chunk_end" || "$d" == "$chunk_end" ]]; do
                for f in adata_logs/*_"$d".csv; do
                    [[ -e "$f" ]] || continue
                    pair=$(basename "$f" | sed 's/^[a-z0-9]*_//; s/\.csv//')
                    grep -qx "$pair" tools/protect_pairs.txt 2>/dev/null || rm -f "$f"
                done
                d=$(date -d "$d +1 day" +%F)
            done
            log "已删除本子波原始 CSV(磁盘余量 $(df -h . | awk 'NR==2{print $4}'))"
        else
            log "⚠️ 有 $((n_done - n_pass - n_inc)) 只次未通过,保留数据待查"
        fi

        # 5. 提交进度
        if git add "$RESULTS" "$UNIVERSE_FILE" 2>/dev/null && \
           git commit -q -m "adata 分波校验: $tag $n_pass/$n_done pass"; then
            log "进度已提交"
        else
            log "无新结果可提交"
        fi

        chunk_start=$(date -d "$chunk_end +1 day" +%F)
    done
    log "✅ 块 $offset 全部日期完成"
done
log "🎉 全部 $UNIVERSE 只 × $START~$END 完成"
