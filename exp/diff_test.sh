#!/usr/bin/env bash
# 差分实验：把 1% 丢包分别施加在「数据方向」（client eth0 出方向）与「ACK 方向」
# （server eth0 出方向），以把限窗配置（配置 B）的停滞缺陷限定到具体路径。
# 用法: bash exp/diff_test.sh [MB]    默认 1MB
# 输出: 四组组合的服务端结果行（stdout；建议重定向到 exp/data/diff_test.log 存档）
# 前置: 脚本会强制 ENABLE_CWND_LIMIT=1（配置 B）编译运行；**无论成败**都在退出时
#       恢复为 0 并重编译（trap EXIT），同时清除两端 tc 整形与 bench 残留进程。
set -u
cd "$(dirname "$0")/.." || exit 1
# 与 run_matrix.sh 共用锁：避免与性能扫描互相切换 ENABLE_CWND_LIMIT 造成数据污染
exec 9>/tmp/tju_exp.lock
flock -n 9 || { echo "另一个实验实例正在运行，退出"; exit 1; }

SRC=src/tju_tcp.c
EXP=exp
MB="${1:-1}"
BYTES=$((MB * 1024 * 1024))
DELAY=6
TC=/vagrant/tju_tcp/test
CL=tju_client
SV=tju_server

restore() {
  local c
  for c in "$CL" "$SV"; do
    docker exec -u root "$c" pkill -9 -f bench_ >/dev/null 2>&1
    docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1
  done
  sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 0/' "$SRC"
  grep -n "define ENABLE_CWND_LIMIT" "$SRC"
  bash $EXP/prep.sh
}
trap restore EXIT

sed -i 's/#define ENABLE_CWND_LIMIT [01]/#define ENABLE_CWND_LIMIT 1/' "$SRC"
echo "### 配置B（ENABLE_CWND_LIMIT=1）, ${MB}MB, 时延 ${DELAY}ms ###"
grep -n "define ENABLE_CWND_LIMIT" "$SRC"
bash $EXP/prep.sh

one_side() { # <container> <loss_pct>
  docker exec -u root "$1" tc qdisc del dev eth0 root >/dev/null 2>&1
  if [ "$2" = "0" ]; then
    # 注意: iproute2 不接受 "loss 0%"（Illegal "loss percent"），丢包为 0 时不得带该参数
    docker exec -u root "$1" tc qdisc add dev eth0 root netem delay "${DELAY}ms" >/dev/null 2>&1
  else
    docker exec -u root "$1" tc qdisc add dev eth0 root netem delay "${DELAY}ms" loss "$2%" >/dev/null 2>&1
  fi
  docker exec -u root "$1" tc qdisc show dev eth0 2>/dev/null | grep -q netem \
    || echo "  !! WARN: netem 未生效 on $1 (loss=$2%)" >&2
}

comb() { # <cli_loss> <srv_loss>
  local cl=$1 sv=$2 out i
  docker exec -u root "$CL" pkill -9 -f bench_ >/dev/null 2>&1
  docker exec -u root "$SV" pkill -9 -f bench_ >/dev/null 2>&1
  sleep 1
  one_side "$CL" "$cl"
  one_side "$SV" "$sv"
  docker exec -u root "$SV" rm -f /tmp/srv.out >/dev/null 2>&1
  docker exec -d -u root "$SV" bash -lc "cd $TC && timeout 400 ./bench_server $BYTES >/tmp/srv.out 2>/dev/null"
  sleep 2
  docker exec -d -u root "$CL" bash -lc "cd $TC && timeout 400 ./bench_client $BYTES >/tmp/cli.out 2>&1"
  out=""
  for i in $(seq 1 100); do
    out=$(docker exec -u root "$SV" cat /tmp/srv.out 2>/dev/null)
    [ -n "$out" ] && break
    # 客户端已退出（40s close 放弃上限）而服务端仍无结果 => 判定为停滞，不再空等
    if [ "$i" -gt 8 ] && [ "$(docker exec -u root "$CL" pgrep -f bench_client 2>/dev/null | wc -l | tr -d ' ')" = "0" ]; then
      out=$(docker exec -u root "$SV" cat /tmp/srv.out 2>/dev/null)
      break
    fi
    sleep 1
  done
  printf 'cli-loss=%s%% srv-loss=%s%% => [%s]\n' "$cl" "$sv" "$out"
}

comb 1 0    # 只丢数据（数据方向丢包）
comb 0 1    # 只丢 ACK
comb 1 1    # 两端都丢
comb 0 0    # 不整形（对照基线）
echo "=== diff-test done ==="
