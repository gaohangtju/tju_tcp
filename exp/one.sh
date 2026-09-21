#!/usr/bin/env bash
# 单点测试: one.sh <loss_pct> <delay_ms> <MB> [rate_mbit=0 表示不限速]
set -u
TC=/vagrant/tju_tcp/test
LOSS="${1:-0}"; DELAY="${2:-6}"; MB="${3:-10}"; RATE="${4:-0}"
BYTES=$((MB * 1024 * 1024))
for c in tju_client tju_server; do
  docker exec -u root "$c" pkill -9 -f bench_ >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1
  if [ "$RATE" -gt 0 ]; then
    docker exec -u root "$c" tc qdisc add dev eth0 root handle 1: htb default 1 >/dev/null 2>&1
    docker exec -u root "$c" tc class add dev eth0 parent 1: classid 1:1 htb rate "${RATE}mbit" >/dev/null 2>&1
    docker exec -u root "$c" tc qdisc add dev eth0 parent 1:1 handle 2: netem delay "${DELAY}ms" loss "${LOSS}%" >/dev/null 2>&1
  else
    docker exec -u root "$c" tc qdisc add dev eth0 root netem delay "${DELAY}ms" loss "${LOSS}%" >/dev/null 2>&1
  fi
done
docker exec -u root tju_client tc qdisc show dev eth0
docker exec -u root tju_server rm -f /tmp/srv.out
docker exec -d -u root tju_server bash -lc "cd $TC && timeout 240 ./bench_server $BYTES >/tmp/srv.out 2>/dev/null"
sleep 1
docker exec -d -u root tju_client bash -lc "cd $TC && timeout 240 ./bench_client $BYTES >/dev/null 2>&1"
line=""
for i in $(seq 1 200); do
  line=$(docker exec -u root tju_server cat /tmp/srv.out 2>/dev/null)
  [ -n "$line" ] && break
  sleep 1
done
docker exec -u root tju_client pkill -9 -f bench_client >/dev/null 2>&1
docker exec -u root tju_server pkill -9 -f bench_server >/dev/null 2>&1
printf 'loss=%s%% delay=%sms rate=%sMbit -> %s\n' "$LOSS" "$DELAY" "$RATE" "$line"
for c in tju_client tju_server; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done
