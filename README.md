# TJU_TCP —— TCP 在应用层的设计与实现

《计算机网络实践-2026》大作业。在 UDP 之上实现一个教学用 TCP（TJU_TCP），
自行完成连接管理、可靠数据传输、流量控制与基础拥塞控制。与标准 TCP 不兼容：
使用自定义 20 字节报文头，仅通过课程给定的 `sendToLayer3` / 内核回调收发。

## 目录结构

```
tju_tcp/
├── Makefile          # 构建：先编 kernel/tju_packet/tju_tcp，再链接 server/client
├── inc/              # 课程给定头文件（global.h / kernel.h / tju_packet.h / tju_tcp.h）
├── src/
│   ├── tju_packet.c  # 报文封装与校验和（课程框架）
│   ├── kernel.c      # 模拟网络层：收发 UDP、按四元组分发（课程框架）
│   ├── server.c      # 服务端应用示例
│   ├── client.c      # 客户端应用示例
│   └── tju_tcp.c     # 全部协议实现所在（连接管理 + 可靠传输 + 流量控制 + 拥塞控制）
├── test/
│   ├── bench_client.c  # 性能实验用发送端（自建基准）
│   └── bench_server.c  # 性能实验用接收端（收满目标字节后打印耗时）
└── exp/                # 性能实验与绘图（第三阶段）
    ├── prep.sh         # 容器内编译工程与基准程序
    ├── run_matrix.sh   # 丢包率/时延扫描器（含 netem 整形与生效性校验）
    ├── run_B.sh        # 切到限窗配置取样后自动切回
    ├── run_all.sh      # 一次性跑完 A+B 四组实验
    ├── plot.py         # 由原始 CSV / trace 生成 fig1…fig4
    ├── data/           # 原始实验数据（CSV）与拥塞窗口 trace
    ├── figures/        # 报告用图表
    └── trace/          # 插桩版源码与 trace 采集脚本（不参与提交编译）
```

详细的复现步骤见 [`REPRODUCE.md`](REPRODUCE.md)。

`src/tju_tcp.c` 中通过 `tju_tcp_t.ext` 指针挂载内部扩展控制块 `struct tju_tcp_ext`，
承载全部协议状态（序号/窗口/定时器/拥塞控制等），不改动 `kernel.c` / `tju_packet.c`。

## 功能

- **连接管理**：三次握手（SYN 重传、重复 SYN/SYNACK 处理）、四次挥手（先后关闭与同时关闭）、
  TIME_WAIT、ISN 生成、连接队列与 `accept`。
- **可靠数据传输**：字节流滑动窗口、累计 ACK、RTT/RTO 估计（RFC 6298 + Karn）、
  超时重传与指数退避、三次重复 ACK 快速重传、32 位序号回绕比较、失序/重复/重叠去重。
- **流量控制**：接收窗口通告（`min(可用, 65535)`）、零窗口探测、窗口恢复更新、基本 SWS 规避。
- **拥塞控制**：基础 Reno（RFC 5681）慢启动 / 拥塞避免 / 快速恢复；
  NewReno（RFC 6582）部分 ACK 快速恢复（挑战任务）。

## 构建与运行

```bash
make            # 生成 client 与 server
```

## 测试

课程自动测试在 client 端执行（需 root，测试脚本会设置 tc 网络整形）：

```bash
cd tju_tcp/test
./test establish    # 三次握手
./test rdt          # 可靠数据传输
./test close        # 四次挥手
```

本地回归结果：`establish_connection` / `reliable_data_transfer` / `close_connection` 均为 100。

## 拥塞控制开关

`src/tju_tcp.c` 顶部：

```c
#define ENABLE_CWND_LIMIT 0
```

- `1`：发送窗口 = `min(rwnd, cwnd)`，启用 Reno/NewReno 限窗（严格 RFC 语义）。
- `0`（默认）：发送窗口 = `rwnd`，仅做流量控制。

说明：课程可靠传输验收链路的丢包率较高（<6%），实测启用 cwnd 限窗会使吞吐显著下降
（在线评分 64.32 → 14.56 → 关闭限窗后 100.00）。定位到的原因是：RTO 后 `cwnd` 降到 1 个
SMSS，而 `FlightSize` 仍为 65535 B，`win = min(rwnd, cwnd)` 使发送方除超时重传外发不出新
数据，进而永远产生不了"新鲜段确认"，`rto_ms` 便永久停留在指数退避后的 4000 ms（详见
`exp/data/cwnd_loss2.log` 与实验报告 §8.3）。故默认取 `0` 以保证可靠传输吞吐；Reno/NewReno
的实现（`cwnd` / `ssthresh` / 部分 ACK 恢复等）完整保留，置 `1` 即可启用并在 trace 中观察。

## 打包提交

课程提交物为 `handin.zip`（`tju_tcp/{src,inc,build,Makefile}`）：

```bash
./pack.sh       # 生成 handin.zip
```

## 说明

- 本仓库为个人独立完成，提交历史与测试数据真实可复现。
- 截止更新（2026-09）：可靠数据传输线上评分口径为 100MB 吞吐（90s 窗口，每 1MB 计 1 分）。
