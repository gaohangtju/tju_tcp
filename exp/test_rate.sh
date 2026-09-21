#!/usr/bin/env bash
# 校验 htb 限速是否真的生效：给定带宽下跑一次 10MB 传输
set -u
TC=/vagrant/tju_tcp/test
RATE="${1:-10}"
for c in tju_client tju_server; do
  docker exec -u root "$c" pkill -9 -f bench_ >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc add dev eth0 root handle 1: htb default 1 >/dev/null 2>&1
  docker exec -u root "$c" tc class add dev eth0 parent 1: classid 1:1 htb rate "${RATE}mbit" >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc add dev eth0 parent 1:1 handle 2: netem delay 6ms loss 0% >/dev/null 2>&1
done
docker exec -u root tju_client tc qdisc show dev eth0
docker exec -u root tju_server rm -f /tmp/srv.out
docker exec -d -u root tju_server bash -lc "cd $TC && timeout 120 ./bench_server 10485760 >/tmp/srv.out 2>/dev/null"
sleep 1
docker exec -d -u root tju_client bash -lc "cd $TC && timeout 120 ./bench_client 10485760 >/dev/null 2>&1"
line=""
for i in $(seq 1 120); do
  line=$(docker exec -u root tju_server cat /tmp/srv.out 2>/dev/null)
  [ -n "$line" ] && break
  sleep 1
done
docker exec -u root tju_client pkill -9 -f bench_client >/dev/null 2>&1
docker exec -u root tju_server pkill -9 -f bench_server >/dev/null 2>&1
echo "RATE=${RATE}mbit  RESULT: $line"
for c in tju_client tju_server; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done
