#!/usr/bin/env bash
# 在容器内重新编译 tju_tcp 与基准测试程序，并打印当前拥塞控制开关状态
set -e
docker exec -u root tju_client bash -lc "cd /vagrant/tju_tcp && make >/dev/null && cd test && \
  gcc -pthread -g -ggdb -DDEBUG -I../inc ./bench_server.c -o bench_server ../build/tju_packet.o ../build/kernel.o ../build/tju_tcp.o && \
  gcc -pthread -g -ggdb -DDEBUG -I../inc ./bench_client.c -o bench_client ../build/tju_packet.o ../build/kernel.o ../build/tju_tcp.o && \
  echo BUILD-OK && grep -n 'define ENABLE_CWND_LIMIT' ../src/tju_tcp.c"
