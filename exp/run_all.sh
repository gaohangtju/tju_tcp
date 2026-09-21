#!/usr/bin/env bash
# 完整跑完两配置的对照实验并恢复配置A源码
#   A = ENABLE_CWND_LIMIT 0（最终提交版：发送许可=rwnd，保留快速重传）
#   B = ENABLE_CWND_LIMIT 1（基础Reno/NewReno 限窗：发送许可=min(rwnd,cwnd)）
# 用法: bash run_all.sh [MB]   默认 4MB
# 说明: 配置A跑满两个变量扫描；配置B在数据丢包链路上会自我锁死（1% 丢包下各档均
#       在客户端 close 放弃上限 40s 内无结果，见 exp/run_B.log），故 B 只取
#       0% 丢包点 3 次，时延扫描该组仅写表头标记缺失。
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=src/tju_tcp.c
BAK=/tmp/tju_tcp.c.cfgA.bak
MB="${1:-4}"
EXP=exp
CSV_HDR='cfg,loss_pct,delay_ms,repeat,target_bytes,recv_bytes,sec,mbps'

echo "===== 配置 A（不限窗）====="
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
(cd $EXP && bash run_matrix.sh A data/exp1_A.csv exp1 "$MB")
(cd $EXP && bash run_matrix.sh A data/exp2_A.csv exp2 "$MB")

echo "===== 切换到配置 B（Reno 限窗）====="
cp "$SRC" "$BAK"
sed -i 's/#define ENABLE_CWND_LIMIT 0/#define ENABLE_CWND_LIMIT 1/' "$SRC"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
(cd $EXP && bash run_matrix.sh B data/exp1_B.csv exp1 "$MB" "0" 3)
echo "$CSV_HDR" > $EXP/data/exp2_B.csv
echo "  exp2_B.csv 仅写表头：限窗版在 1% 丢包下每点均停滞，无法取样（见 exp/run_B.log）"

echo "===== 恢复配置 A 源码并重编译 ====="
cp "$BAK" "$SRC"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
echo ALL-DONE
