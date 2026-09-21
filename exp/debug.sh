#!/usr/bin/env bash
set -u
TC=/vagrant/tju_tcp/test
docker exec -u root tju_client pkill -9 -f bench_ 2>/dev/null
docker exec -u root tju_server pkill -9 -f bench_ 2>/dev/null
docker exec -u root tju_client tc qdisc del dev eth0 root 2>/dev/null
docker exec -u root tju_server tc qdisc del dev eth0 root 2>/dev/null
echo "== start server (detached) =="
docker exec -u root tju_server rm -f /tmp/srv.out /tmp/srv.err
docker exec -d -u root tju_server bash -lc "cd $TC && timeout 60 ./bench_server 1048576 >/tmp/srv.out 2>/tmp/srv.err"
sleep 2
echo "== server procs =="
docker exec -u root tju_server bash -lc "ps -eo comm | grep bench || echo none"
echo "== client run =="
docker exec -u root tju_client bash -lc "cd $TC && timeout 60 ./bench_client 1048576"
echo "client rc=$?"
sleep 2
echo "== srv.out =="; docker exec -u root tju_server cat /tmp/srv.out
echo "== srv.err(head) =="; docker exec -u root tju_server head -5 /tmp/srv.err
