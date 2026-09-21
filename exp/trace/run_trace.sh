#!/usr/bin/env bash
# 采集拥塞窗口时间序列：用「限窗(Reno)」插桩客户端发送，记录 cwnd/ssthresh/在途/序号
# 用法: run_trace.sh <loss_pct> <out.log>
set -u
TC=/vagrant/tju_tcp/test
LOSS="${1:-2}"
OUT="${2:-/tmp/cwnd.log}"

docker exec -u root tju_client pkill -9 -f bench_ 2>/dev/null
docker exec -u root tju_client pkill -9 -f trace_client 2>/dev/null
docker exec -u root tju_server pkill -9 -f bench_ 2>/dev/null
for c in tju_client tju_server; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done

echo "== 构建插桩客户端（限窗 Reno 版，仅实验用） =="
docker exec -u root tju_client bash -lc "cd $TC && sed 's/#define ENABLE_CWND_LIMIT 0/#define ENABLE_CWND_LIMIT 1/' ../exp/trace/tju_tcp_trace.c > /tmp/tr.c && \
  gcc -pthread -g -ggdb -DDEBUG -I../inc -c /tmp/tr.c -o /tmp/tr.o && \
  gcc -pthread -g -ggdb -DDEBUG -I../inc ./bench_client.c -o /tmp/trace_client /tmp/tr.o ../build/tju_packet.o ../build/kernel.o && echo TRACE-BUILD-OK"

for c in tju_client tju_server; do
  # 注意: iproute2 不接受 "loss 0%"（Illegal "loss percent"）；丢包为 0 时不得带该参数
  if [ "$LOSS" = "0" ]; then
    docker exec -u root "$c" tc qdisc add dev eth0 root netem delay 6ms rate 100mbit >/dev/null 2>&1
  else
    docker exec -u root "$c" tc qdisc add dev eth0 root netem delay 6ms loss "${LOSS}%" rate 100mbit >/dev/null 2>&1
  fi
  if ! docker exec -u root "$c" tc qdisc show dev eth0 2>/dev/null | grep -q netem; then
    echo "  !! WARN: netem 未生效 on $c (loss=${LOSS})" >&2
  fi
done

docker exec -u root tju_server rm -f /tmp/srv.out
docker exec -d -u root tju_server bash -lc "cd $TC && timeout 200 ./bench_server 10485760 >/tmp/srv.out 2>/dev/null"
sleep 1
docker exec -u root tju_client bash -lc "cd /tmp && timeout 200 ./trace_client 10485760 2>/tmp/cwnd.log >/dev/null"

sleep 1
echo -n "  服务端: "; docker exec -u root tju_server cat /tmp/srv.out
echo -n "  样本行数: "; docker exec -u root tju_client wc -l /tmp/cwnd.log
docker cp tju_client:/tmp/cwnd.log "$OUT" >/dev/null 2>&1
for c in tju_client tju_server; do docker exec -u root "$c" tc qdisc del dev eth0 root >/dev/null 2>&1; done
echo "trace saved: $OUT"
