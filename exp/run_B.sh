#!/usr/bin/env bash
# 只跑配置 B（基础Reno 限窗）的对照取样，结束后恢复配置 A（ENABLE_CWND_LIMIT=0）并重编译
# 用法: bash run_B.sh [MB]   默认 4MB
# 说明: 限窗版在**数据丢包**链路上会自我锁死：1% 丢包下 4MB/2MB/1MB 均在客户端
#       close 放弃上限（40s）内无结果（原始尝试记录见 exp/run_B.log）。故：
#         - exp1（丢包率扫描）只取 0% 丢包点，重复 3 次；
#         - exp2（时延扫描）各点均落在 1% 丢包上、全部停滞，不取样，仅写表头标记缺失。
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=src/tju_tcp.c
EXP=exp
MB="${1:-4}"
CSV_HDR='cfg,loss_pct,delay_ms,repeat,target_bytes,recv_bytes,sec,mbps'

sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 1/' "$SRC"
echo "===== 配置 B（Reno 限窗）====="
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
(cd $EXP && bash run_matrix.sh B data/exp1_B.csv exp1 "$MB" "0" 3)
echo "$CSV_HDR" > $EXP/data/exp2_B.csv
echo "  exp2_B.csv 仅写表头：限窗版在 1% 丢包下每点均停滞，无法取样（见 exp/run_B.log）"

echo "===== 恢复配置 A 源码并重编译 ====="
sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 0/' "$SRC"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
echo B-DONE
