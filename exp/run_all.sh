#!/usr/bin/env bash
# 完整跑完两配置的对照实验并恢复配置A源码
#   A = ENABLE_CWND_LIMIT 0（最终提交版：发送许可=rwnd，保留快速重传）
#   B = ENABLE_CWND_LIMIT 1（基础Reno/NewReno 限窗：发送许可=min(rwnd,cwnd)）
# 用法: bash run_all.sh [MB]   默认 4MB
# 说明: 配置A跑满两个变量扫描；配置B在丢包链路上吞吐塌陷（约0.16 MB/s），
#       故只在各扫描的前两个点取样（0/1% 丢包、6/20ms 时延），保证单轮能在
#       客户端 close 放弃上限（40s）内完成。
set -u
cd "$(dirname "$0")/.." || exit 1
SRC=src/tju_tcp.c
BAK=/tmp/tju_tcp.c.cfgA.bak
MB="${1:-4}"
EXP=exp

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
(cd $EXP && bash run_matrix.sh B data/exp1_B.csv exp1 "$MB" "0 1" 2)
(cd $EXP && bash run_matrix.sh B data/exp2_B.csv exp2 "$MB" "6 20" 2)

echo "===== 恢复配置 A 源码并重编译 ====="
cp "$BAK" "$SRC"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh
echo ALL-DONE
