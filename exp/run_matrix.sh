#!/usr/bin/env bash
# TJU_TCP 第三阶段性能对照实验
# 用法: run_matrix.sh <配置标签> <输出CSV> <exp1|exp2> [MB] [点列表] [重复次数]
#   exp1 变量=丢包率: 固定时延6ms, 默认丢包 {0,1,2,5,10}%, 每点重复3次
#   exp2 变量=时延  : 固定丢包1%, 默认时延 {6,20,50,100}ms, 每点重复2次
# 整形: netem delay(+loss) 施加于收发双方 eth0；loss=0% 时 tc 不接受 "loss 0%" 语法, 只加时延
# 说明: 服务端收满 target 即退出, 客户端 close 会长时间等待; 故轮询服务端结果后本轮直接结束
set -u
# 独占锁：防止本脚本被重复启动导致数据互相覆盖
exec 9>/tmp/tju_exp.lock
flock -n 9 || { echo "另一个实验实例正在运行，退出"; exit 1; }

CFG="${1:?usage: run_matrix.sh <cfg-label> <out.csv> <exp1|exp2> [MB] [points] [reps]}"
OUT="${2:?output csv}"
MODE="${3:?mode exp1|exp2}"
MB="${4:-1}"
BYTES=$((MB * 1024 * 1024))
TC=/vagrant/tju_tcp/test
CL=tju_client
SV=tju_server

if [ "$MODE" = "exp1" ]; then
  POINTS="${5:-0 1 2 5 10}"; REPS="${6:-3}"; FIXDELAY=6
else
  POINTS="${5:-6 20 50 100}"; REPS="${6:-2}"; FIXLOSS=1
fi

echo "cfg,loss_pct,delay_ms,repeat,target_bytes,recv_bytes,sec,mbps" > "$OUT"

shape() { # loss delay
  local c
  for c in "$CL" "$SV"; do
    docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1
    if [ "${1}" = "0" ]; then
      docker exec -u root "$c" tc qdisc add dev eth0 root netem delay "${2}ms" >/dev/null 2>&1
    else
      docker exec -u root "$c" tc qdisc add dev eth0 root netem delay "${2}ms" loss "${1}%" >/dev/null 2>&1
    fi
    # 校验整形确实生效（netem 未被静默拒绝），否则本轮数据不可信
    if ! docker exec -u root "$c" tc qdisc show dev eth0 2>/dev/null | grep -q netem; then
      echo "  !! WARN: netem 未生效 on $c (loss=${1} delay=${2})" >&2
    fi
  done
}
unshape() {
  local c
  for c in "$CL" "$SV"; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done
}
nproc_in() { # container -> number of bench_ processes (always prints a clean integer)
  docker exec -u root "$1" pgrep -f bench_ 2>/dev/null | wc -l | tr -d ' '
}
kill_old() {
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
    docker exec -u root "$CL" pkill -9 -f bench_ >/dev/null 2>&1
    docker exec -u root "$SV" pkill -9 -f bench_ >/dev/null 2>&1
    [ "$(nproc_in "$CL")" = "0" ] && [ "$(nproc_in "$SV")" = "0" ] && return 0
    sleep 0.5
  done
  echo "  !! WARN: bench 进程清理未干净" >&2
}

attempt() { # loss delay rep -> 回显服务端结果行；失败回显空
  local loss=$1 delay=$2 rep=$3
  kill_old; unshape; shape "$loss" "$delay"

  docker exec -u root "$SV" rm -f /tmp/srv.out >/dev/null 2>&1
  docker exec -u root "$CL" rm -f /tmp/cli.out >/dev/null 2>&1
  docker exec -d -u root "$SV" bash -lc "cd $TC && timeout 400 ./bench_server $BYTES >/tmp/srv.out 2>/dev/null"
  sleep 2
  # 校验服务端确实起来了（避免旧进程占端口导致假失败）
  if [ "$(docker exec -u root "$SV" pgrep -f bench_server 2>/dev/null | wc -l | tr -d ' ')" = "0" ]; then
    echo "  !! WARN: 服务端未能启动 (loss=$loss delay=$delay rep=$rep)" >&2
    return 0
  fi
  docker exec -d -u root "$CL" bash -lc "cd $TC && timeout 400 ./bench_client $BYTES >/tmp/cli.out 2>&1"

  local line="" i
  for i in $(seq 1 150); do
    line=$(docker exec -u root "$SV" cat /tmp/srv.out 2>/dev/null)
    [ -n "$line" ] && break
    # 客户端已退出而服务端仍无结果 => 本轮无效，提前放弃（不再空等）
    if [ "$(docker exec -u root "$CL" pgrep -f bench_client 2>/dev/null | wc -l | tr -d ' ')" = "0" ] && [ "$i" -gt 8 ]; then
      line=$(docker exec -u root "$SV" cat /tmp/srv.out 2>/dev/null)
      [ -n "$line" ] && break
      echo "  !! WARN: 客户端已退出但服务端无结果 (loss=$loss delay=$delay rep=$rep)，重试" >&2
      break
    fi
    sleep 1
  done
  kill_old
  echo "$line"
}

run_one() { # loss delay rep
  local loss=$1 delay=$2 rep=$3 line
  line=$(attempt "$loss" "$delay" "$rep")
  # 空白结果重试一次（一次性的组网/端口竞争偶发失败）
  if [ -z "$(echo "$line" | tr -d '[:space:]')" ]; then
    sleep 1
    line=$(attempt "$loss" "$delay" "$rep")
  fi

  local recv sec mbps
  recv=$(echo "$line" | sed -n 's/.*recv=\([0-9]*\) bytes.*/\1/p')
  sec=$(echo "$line" | sed -n 's/.*in \([0-9.]*\) s.*/\1/p')
  [ -z "$recv" ] && recv=0
  [ -z "$sec" ] && sec=0
  mbps=$(awk -v b="$recv" -v t="$sec" 'BEGIN{ if(t>0) printf "%.4f", b/t/1e6; else printf "0" }')
  echo "$CFG,$loss,$delay,$rep,$BYTES,$recv,$sec,$mbps" >> "$OUT"
  printf '  [%s loss=%s%% delay=%sms rep=%s] recv=%s sec=%s => %s MB/s\n' "$CFG" "$loss" "$delay" "$rep" "$recv" "$sec" "$mbps"
  unshape
}

for p in $POINTS; do
  for r in $(seq 1 "$REPS"); do
    if [ "$MODE" = "exp1" ]; then run_one "$p" "$FIXDELAY" "$r"; else run_one "$FIXLOSS" "$p" "$r"; fi
  done
done
kill_old; unshape
echo "=== done: $OUT ==="
