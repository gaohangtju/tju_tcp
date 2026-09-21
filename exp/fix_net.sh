#!/usr/bin/env bash
# 修复容器自定义网络（免 sudo）：VM 挂起/恢复后网桥会 DOWN
set -u
docker network disconnect -f netproj tju_server 2>/dev/null
docker network disconnect -f netproj tju_client 2>/dev/null
docker network rm netproj 2>/dev/null
docker network create --subnet 172.17.0.0/16 --gateway 172.17.0.1 netproj
docker network connect --ip 172.17.0.3 netproj tju_server
docker network connect --ip 172.17.0.2 netproj tju_client
sleep 2
ip -br link show type bridge
docker exec -u root tju_client ping -c2 -W2 172.17.0.3 && echo "NET-OK" || echo "NET-STILL-BAD"
