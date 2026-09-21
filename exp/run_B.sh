#!/usr/bin/env bash
# 只跑配置 B（基础Reno 限窗）的对照取样，结束后恢复配置 A（ENABLE_CWND_LIMIT=0）并重编译
# 用法: bash run_B.sh [MB]   默认 4MB
# 说明: 限窗版在丢包链路吞吐塌陷（约0.16 MB/s），故只在各扫描的前两个点取样
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=src/tju_tcp.c
EXP=exp
MB="${1:-4}"

sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 1/' "$SRC"
echo "===== 配置 B（Reno 限窗）====="
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
(cd $EXP && bash run_matrix.sh B data/exp1_B.csv exp1 "$MB" "0 1" 2)
(cd $EXP && bash run_matrix.sh B data/exp2_B.csv exp2 "$MB" "6 20" 2)

echo "===== 恢复配置 A 源码并重编译 ====="
sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 0/' "$SRC"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
echo B-DONE
