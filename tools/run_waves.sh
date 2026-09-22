#!/bin/bash
# 分波驱动(流水版):股票块(200只) × 周日历块(≈5 个交易日) 双层分波
#   校验第 N 波的同时后台预取第 N+1 波(独立暂存目录),拉取时间被校验时间覆盖
#   每子波:入库 → 校验 → 全过则删原始数据 → 提交结果 → 下一子波
# 用法: nohup bash tools/run_waves.sh > logs/waves.log 2>&1 &
# 进度: tail -f logs/waves.log(明细 logs/fetch.log、logs/validate.log)
# 结果: result/adata_validation_results.csv(增量,断点续传; 2026-09-22起不再逐子波提交)
# 断点: logs/waves.state 记录最后完成子波,重启自动跳到下一子波;
#       暂存目录(adata_staging/)残留自恢复,被中断的拉取断点续传
set -uo pipefail
cd "$(dirname "$0")/.."
export TZ=Asia/Shanghai   # 系统时钟是 UTC,日志时间戳统一按东八区输出

FETCH_PY=/home/donghuale/venv_adata/bin/python
VENV_PY=.venv/bin/python
START=2024-10-08
END=2024-12-31
UNIVERSE=1000
UNIVERSE_FILE=adata_universe_1000.txt
BLOCK_SIZE=200         # 股票块大小(5 块 × 200 = 1000)
CHUNK_DAYS=7           # 日历天/子波(≈5 个交易日)
RESULTS=result/adata_validation_results.csv
STATE=logs/waves.state   # 最后完成的子波 "<offset> <chunk_start>"
STAGING=adata_staging    # 预取暂存根目录(每子波一个子目录,校验目录不受污染)

log() { echo "[$(date '+%m-%d %H:%M:%S')] $*"; }
disk_free() { df -h . | awk 'NR==2{print $4}'; }

# ---- 生成完整子波清单 "offset chunk_start chunk_end"(执行顺序=清单顺序) ----
waves=()
for (( offset=0; offset<UNIVERSE; offset+=BLOCK_SIZE )); do
    cs=$START
    while [[ "$cs" < "$END" ]]; do
        ce=$(date -d "$cs +$((CHUNK_DAYS-1)) days" +%F)
        [[ "$ce" > "$END" ]] && ce=$END
        waves+=("$offset $cs $ce")
        cs=$(date -d "$ce +1 day" +%F)
    done
done
TOTAL=${#waves[@]}

# ---- 断点:状态文件记录最后完成的子波,从它的下一个开始 ----
start_idx=0
if [[ -f "$STATE" ]]; then
    read -r s_off s_chunk < "$STATE"
    start_idx=-1
    for i in "${!waves[@]}"; do
        read -r w_off w_cs _ <<< "${waves[$i]}"
        if (( w_off == s_off )) && [[ "$w_cs" == "$s_chunk" ]]; then
            start_idx=$((i + 1))
            log "断点续跑:跳过前 $start_idx 个子波(已完成至 块$s_off @ $s_chunk)"
            break
        fi
    done
    # 状态文件与清单对不上时宁可报错,也不能从头重拉(已完成子波数据已删)
    if (( start_idx < 0 )); then
        log "❌ 状态文件内容($s_off $s_chunk)不在子波清单中,请检查后人工处理"
        exit 1
    fi
fi

# fetch_wave <idx> [out_dir]:拉取该子波到指定目录(默认其专属暂存目录)
# 四件套断点续传:已齐的只次自动跳过
fetch_wave() {
    local i=$1 out=${2:-}
    read -r offset cs ce <<< "${waves[$i]}"
    [[ -z "$out" ]] && out="$STAGING/${offset}_${cs}"
    mkdir -p "$out"
    $FETCH_PY fetch_adata_logs.py --start "$cs" --end "$ce" \
        --universe-size $UNIVERSE --offset $offset --limit $BLOCK_SIZE \
        --universe-file "$UNIVERSE_FILE" --out "$out" >> logs/fetch.log 2>&1
}

# stage_in <idx>:暂存文件并入校验目录 adata_logs(同分区 rename,秒级)
stage_in() {
    local i=$1 dir
    read -r offset cs _ <<< "${waves[$i]}"
    dir="$STAGING/${offset}_${cs}"
    [[ -d "$dir" ]] || return 0
    find "$dir" -maxdepth 1 -name '*.csv' -exec mv -t adata_logs {} +
    rmdir "$dir" 2>/dev/null || true
}

# ---- 首波:直接拉进校验目录(重启续跑时已入库文件自动跳过,不重拉) ----
if (( start_idx < TOTAL )); then
    read -r o cs ce <<< "${waves[$start_idx]}"
    log "首波拉取 块$o-$((o+BLOCK_SIZE-1)) @ $cs~$ce(磁盘余量 $(disk_free))..."
    until fetch_wave $start_idx adata_logs; do
        log "❌ 拉取失败,见 logs/fetch.log,60s 后重试"
        sleep 60
    done
fi

for (( i=start_idx; i<TOTAL; i++ )); do
    read -r offset chunk_start chunk_end <<< "${waves[$i]}"
    tag="块$offset-$((offset+BLOCK_SIZE-1)) @ $chunk_start~$chunk_end"
    log "===== 子波 $tag ($((i+1))/$TOTAL) ====="

    # 1. 后台预取下一子波(独立暂存目录,与本波校验并行;失败自动重试)
    prefetch_pid=""
    if (( i+1 < TOTAL )); then
        read -r no ncs nce <<< "${waves[$i+1]}"
        log "后台预取 块$no-$((no+BLOCK_SIZE-1)) @ $ncs~$nce"
        ( until fetch_wave $((i+1)); do sleep 60; done ) &
        prefetch_pid=$!
    fi

    # 2. 校验本波(增量:已 pass 的只次自动跳过;异常退出重试,不丢只次)
    log "校验中(磁盘余量 $(disk_free))..."
    until $VENV_PY tests/price_cage/run_adata_validation.py --batch-size 12 >> logs/validate.log 2>&1; do
        log "❌ 校验进程异常退出,见 logs/validate.log;保留数据,60s 后重试"
        sleep 60
    done

    # 3. 本波只次状态统计(同一 (sym,date) 取最后一次记录;
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

    # 4. 全部 pass/data_incomplete 才删本波原始数据(其余保留待查;
    #    跳过 tools/protect_pairs.txt 里的回归基准只次;
    #    预取的下一波在暂存目录,不受删除影响;重叠波次日期区间必然不相交)
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
        log "已删除本子波原始 CSV(磁盘余量 $(disk_free))"
    else
        log "⚠️ 有 $((n_done - n_pass - n_inc)) 只次未通过,保留数据待查"
    fi

    # 5. 提交进度(2026-09-22 起停用: 避免历史噪音, 结果 CSV 于周期收尾时手动汇总提交)

    # 6. 记录断点;等预取完成,入库作为下一波校验数据
    echo "$offset $chunk_start" > "$STATE"
    if [[ -n "$prefetch_pid" ]]; then
        if wait $prefetch_pid; then
            stage_in $((i+1))
            log "下一子波数据已就绪(磁盘余量 $(disk_free))"
        else
            log "⚠️ 预取异常,下一子波将前台重拉(暂存断点续传)"
        fi
    fi
done
log "🎉 全部 $UNIVERSE 只 × $START~$END 完成($TOTAL 子波)"
