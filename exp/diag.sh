#!/usr/bin/env bash
set -u
echo "=== 容器内地址 ==="
docker exec -u root tju_client bash -lc "ip -br addr show eth0; echo '--hosts--'; cat /etc/hosts"
docker exec -u root tju_server bash -lc "ip -br addr show eth0"
echo "=== 连通性 client -> server ==="
docker exec -u root tju_client ping -c2 -W2 172.17.0.3 || echo "ping failed"
echo "=== 宿主机网桥 ==="
ip -br link show type bridge
ip -br addr show | grep -E "br-|netproj" || true
echo "=== netproj 详情 ==="
docker network inspect netproj --format "{{range .Containers}}{{.Name}} {{.IPv4Address}}{{println}}{{end}}" 2>&1
