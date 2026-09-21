#!/usr/bin/env bash
set -u
TC=/vagrant/tju_tcp/test
for c in tju_client tju_server; do
  docker exec -u root "$c" pkill -9 -f bench_ >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1
  docker exec -u root "$c" tc qdisc add dev eth0 root netem delay 6ms loss 1% rate 100mbit >/dev/null 2>&1
done
docker exec -u root tju_server rm -f /tmp/srv.out >/dev/null 2>&1
docker exec -d -u root tju_server bash -lc "cd $TC && timeout 150 ./bench_server 10485760 >/tmp/srv.out 2>/dev/null"
sleep 1
docker exec -u root tju_client bash -lc "cd $TC && timeout 150 ./bench_client 10485760" >/dev/null 2>&1
sleep 2
echo "--- server ---"
docker exec -u root tju_server cat /tmp/srv.out 2>/dev/null
for c in tju_client tju_server; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done
