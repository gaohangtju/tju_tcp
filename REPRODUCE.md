# 复现说明（REPRODUCE）

> 本文件说明如何从零复现本仓库的**构建、功能测试、性能实验、拥塞窗口 trace 与报告图表**。
> 所有实验数据、日志与图表均由本仓库脚本在课程容器环境中真实运行产生，可逐条重跑。

---

## 1. 环境与前置条件

| 项 | 值 |
|---|---|
| 虚拟机 | Ubuntu（工程镜像位于 `~/Desktop/计网大作业/code`，容器内挂载为 `/vagrant`） |
| 容器 | `tju_client`（本地 172.17.0.2）、`tju_server`（本地 172.17.0.3），Docker 网络 `netproj`，两者均挂载同一目录 |
| 权限 | 用户需在 `docker` 组（免 sudo 直接 `docker exec`）；网络整形必须以 `-u root` 执行 |
| 依赖 | `gcc`、`make`、`pthread`、`iproute2`（`tc`/`netem`）、`python3` + `Pillow`（仅绘图需要，不依赖 matplotlib） |
| 触发条件 | 容器启动后需确认 `server` 容器 sshd 与网桥正常（见文末排障） |

工程编译只涉及 `src/tju_packet.c`、`src/kernel.c`、`src/tju_tcp.c`，链接 `src/server.c` / `src/client.c`。

## 2. 目录与文件对应关系

| 文件 | 作用 |
|---|---|
| `src/tju_tcp.c` | 全部协议实现（连接管理、可靠传输、流量控制、基础 Reno / NewReno） |
| `src/kernel.c` | 课程框架（仅 `cal_hash` 可改） |
| `src/tju_packet.c` | 课程框架（不可改） |
| `test/bench_client.c` | 性能实验用发送端（自建基准，非课程提交内容） |
| `test/bench_server.c` | 性能实验用接收端，收满目标字节后打印 `recv=<B> bytes in <s> s` |
| `exp/prep.sh` | 容器内 `make` + 编译 `bench_{server,client}`，并回显 `ENABLE_CWND_LIMIT` 当前值 |
| `exp/run_matrix.sh` | 扫描器：`run_matrix.sh <配置标签> <输出CSV> <exp1\|exp2> [MB] [点列表] [重复次数]` |
| `exp/run_B.sh` | 切到 `ENABLE_CWND_LIMIT=1` → 编译 → 跑配置 B（`exp1_B.csv` 取 0% 丢包点 3 次；`exp2_B.csv` 只写表头）→ 切回 0 → 重编译 |
| `exp/run_all.sh` | 一次性跑完 A+B 四组实验（完整复现用） |
| `exp/run_B.log` | 配置 B 的原始运行日志（含 1% 丢包点停滞的 `WARN` 与 `recv=0` 记录，是"数据丢包后停滞"的原始证据） |
| `exp/trace/tju_tcp_trace.c` | 由 `src/tju_tcp.c` 机械派生的**插桩版**（仅打印 `[CWND]` 事件，不参与提交编译） |
| `exp/trace/run_trace.sh` | 构建插桩客户端并采集 trace |
| `exp/plot.py` | 读 CSV/日志生成 `exp/figures/fig1…fig4` |
| `exp/data/*.csv` | 原始实验数据 |
| `exp/data/cwnd_*.log` | 原始 `[CWND]` trace |
| `exp/figures/*.png` | 报告使用的四张图 |

**数据 → 图表**：`exp1_A.csv`+`exp1_B.csv` → 图 1；`exp2_A.csv`+`exp2_B.csv` → 图 2；`cwnd_loss2.log` → 图 3；`cwnd_loss0.log` → 图 4。

## 3. 构建

```bash
bash exp/prep.sh
# 期望输出：BUILD-OK 与 "#define ENABLE_CWND_LIMIT 0"
```

## 4. 功能测试（课程自动测试）

```bash
docker exec -u root tju_client bash -lc "cd /vagrant/tju_tcp/test && ./test establish"
docker exec -u root tju_client bash -lc "cd /vagrant/tju_tcp/test && ./test rdt"
docker exec -u root tju_client bash -lc "cd /vagrant/tju_tcp/test && ./test close"
```

期望：三场景各 100 分（`{"scores":{...,"reliable_data_transfer":100,...}}`）。

> 注意：**不要**在 `tju_tcp/` 根目录放置 `test_Makefile`，否则测试脚本会走 `cp test_Makefile test/Makefile` 分支并在其内部报错。

## 5. 性能实验

```bash
# 配置 A（最终提交版，win = rwnd）——两组变量扫描，约 3 分钟
(cd exp && bash run_matrix.sh A data/exp1_A.csv exp1 4)   # 丢包率 0/1/2/5/10%，6ms，3 次
(cd exp && bash run_matrix.sh A data/exp2_A.csv exp2 4)   # 时延 6/20/50/100ms，1% 丢包，2 次

# 配置 B（基础 Reno 限窗，win = min(rwnd, cwnd)）——自动切宏/编译/取样/切回
bash exp/run_B.sh 4
```

**实验设计**：单次传输 4 MB，吞吐率 = 服务端 `recv_bytes / sec`；每点重复若干次并报均值与全部原始值。**无随机种子**——netem 丢包由内核随机决定，不可复现，故用重复次数反映抖动。

**重要注意事项（踩过的坑）**：

1. `tc ... netem ... loss 0%` 是 **iproute2 非法写法**（`Illegal "loss percent"`），整条命令会失败。脚本在丢包为 0 时只加时延，并在整形后用 `tc qdisc show dev eth0 | grep netem` **校验整形确实生效**，否则打印 `WARN`。
2. **不要把关键命令的 stderr 重定向到 `/dev/null`**——错误会被静默吞掉，实验会"看起来成功但实际没整形"。若结果超出理论边界（如吞吐 > `rwnd/RTT`），首先怀疑整形未生效。
3. 每轮前后都要清理残留进程（`pkill -9 -f bench_`），残留的服务端会占用端口导致假失败；`run_matrix.sh` 已内置带校验的清理循环。
4. 配置 B 在**数据丢包**链路上会自我锁死（1% 丢包下 4 MB/2 MB/1 MB 均在客户端 40 s 关闭上限内无法完成）：`run_B.sh` 因此对 `exp1_B.csv` 只取 0% 丢包点（3 次），对 `exp2_B.csv` 只写表头标记缺失。原始尝试记录见 `exp/run_B.log`（同一取样点连续两次 `客户端已退出但服务端无结果` 重试后 `recv=0`）。这是**实测结论**，不是脚本故障，详见报告 §8.3。

## 6. 拥塞窗口 trace（图 3 / 图 4）

```bash
bash exp/trace/run_trace.sh 2 /tmp/cwnd_loss2.log   # 2% 丢包 → 图 3
bash exp/trace/run_trace.sh 0 /tmp/cwnd_loss0.log   # 无丢包 → 图 4
# 把结果放到数据目录供绘图脚本读取
cp /tmp/cwnd_loss2.log /tmp/cwnd_loss0.log exp/data/
```

> 插桩版由 `src/tju_tcp.c` 派生，强制 `ENABLE_CWND_LIMIT=1` 以便观察窗口对发送的实际约束；**它不属于提交代码**，仅在实验中使用。

## 7. 出图

```bash
python exp/plot.py      # 生成 exp/figures/fig1_loss.png … fig4_cwnd_loss0.png
```

Pillow 缺失时：`pip install Pillow`。图内中文使用系统字体（微软雅黑 / 黑体 / 宋体），Linux 下如缺字体会回退到默认字体。

## 8. 报告 Word 文档

报告 Markdown 源在 `report/stage3/实验报告-第三阶段.md`；由 `report/scripts` 中的生成脚本渲染为 `3024244208_高航_第3周课程报告.docx`（保留课程模板封面、阶段索引表与样式，正文按标题/表格/代码块/图片逐块渲染）。

## 9. 排障备查

- 容器网络故障（`NoValidConnectionsError ... 172.17.0.3:22`）：多为 `netproj` 网桥 DOWN 或 server 容器 sshd 未启动。重建网络并固定 IP，再 `docker exec tju_server bash -lc "service ssh start"`。
- `paramiko` 连不上服务端时，先用 `docker exec tju_client ping -c3 172.17.0.3` 判断连通性（server 镜像**无 ping**，对它用 `ss -lntp | grep :22`）。
- 上一轮实验残留的 tc 整形会让后续结果全部失真，务必 `docker exec -u root <容器> tc qdisc del dev eth0 root`。
- 服务端日志若出现 `期望的 ACK 序号比实际大 1` 之类的提示，优先检查 FIN/SYN 是否按 1 个序号推进。
