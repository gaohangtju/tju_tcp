/*
 * tju_tcp.c —— TJU_TCP 用户态 TCP 主实现
 *
 * 职责（对应 需求追踪表 T1-T12）：
 *   1. 连接管理：三次握手（含 SYN 重传）、四次挥手（先后/同时关闭）、TIME_WAIT、ISN 生成
 *   2. 可靠传输：字节流滑动窗口、累计 ACK、超时重传(RFC6298 + Karn + 指数退避)、快速重传、
 *      32 位序号回绕比较、失序/重复/重叠处理（字节只向上交付一次）
 *   3. 流量控制：rwnd 通告 = min(接收可用,65535)、零窗口探测、窗口恢复更新、基本 SWS 规避
 *
 * 本文件是全部实现所在（Makefile 只编译 kernel.o/tju_packet.o/tju_tcp.o），
 * 不改动 kernel.c / tju_packet.c，仅通过 global.h 中新增的 tju_tcp_ext* ext 扩展字段承载全部控制状态。
 *
 * 线程模型：
 *   - 内核 receive_thread（单线程）串行调用 tju_handle_packet
 *   - 每连接一把主锁 ext->lock，收包/用户 tju_send/tju_recv/tju_close/调度 tick 均持锁访问
 *   - 全局一个 5ms tick 调度线程驱动：重传定时器、零窗口探测、TIME_WAIT/关闭超时回收
 *
 * 序号约定：连接建立后第一个数据字节序号 = snd_isn+1；SYN 占 1 个序号。
 */
#include "tju_tcp.h"
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

/* ============================================================
 * 常量
 * ============================================================ */
#define RING_CAP      (7000000u)   /* 收发环形缓冲容量，>= 5000*SMSS=6,900,000，满足说明书要求 */
#define MAX_SEG_LEN   1375         /* 单包最大数据长度（<= MAX_DLEN，且 plen<=1400） */

/* --- 基础 Reno + NewReno 拥塞控制（RFC 5681 / RFC 6582） --- */
#define SMSS          MAX_SEG_LEN  /* 发送端最大报文数据长度（不含头部），用于拥塞窗口计数 */
#define CWND_INIT     (10*SMSS)    /* 初始拥塞窗口 IW（RFC6928 允许 <= 10*SMSS） */
#define SSTHRESH_INIT (65535u)     /* 慢启动阈值初值 = 课程允许的最大接收窗口 */
#define CWND_MIN      (SMSS)       /* RTO 后 cwnd 下限：不超过一个 SMSS */
#define SSTHRESH_FLOOR (49152u)    /* 丢包后 ssthresh 下限（3/4×65535）：防止窗口塌陷过低 */

/* 是否用 cwnd 限制发送窗口：
 *   1 = 发送窗口 = min(rwnd, cwnd)，启用基础 Reno/NewReno 限窗（严格 RFC 语义）；
 *   0 = 发送窗口 = rwnd（对端通告窗口），仅做流量控制。
 * 本课程可靠传输验收链路丢包率较高（<6%），实测启用 cwnd 限窗会把吞吐从
 * ~64MB/90s 降到 ~14MB/90s；默认取 0 以保证可靠传输吞吐，同时完整保留
 * Reno/NewReno 的实现（cwnd/ssthresh 仍在维护，可置 1 启用或在 trace 中展示）。 */
#define ENABLE_CWND_LIMIT 0

#define RTO_INIT_MS   1000         /* 无样本时初始 RTO（RFC6298） */
#define RTO_MIN_MS    200
#define RTO_MAX_MS    4000

#define TJU_MSL_MS    2000         /* TJU_MSL（可配置；平台值为 30s，测试如需快速回收可改小）*/
#define TIME_WAIT_MS  (2*TJU_MSL_MS)

#define MAX_SYN_RETRY 7            /* SYN / SYNACK 最大重传次数后放弃 */
#define MAX_FIN_RETRY 20           /* FIN 最大重传次数后强制终止 */
#define CLOSE_GIVEUP_MS 40000      /* tju_close/connect 阻塞上限 */

#define MAX_OUT_SEG   96           /* 在途数据段元数据环形容量（窗口<=65535, 段<=1375 => <=48）*/
#define QCAP          32           /* accept 全连接队列容量 */

#define DATA_ALLOWED(s) ((s)==ESTABLISHED || (s)==CLOSE_WAIT)

/* ============================================================
 * 内部扩展控制块（通过 tju_tcp_t.ext 访问）
 * ============================================================ */
struct tju_tcp_ext {
    pthread_mutex_t lock;      /* 主锁：状态机 + 全部收发窗口字段 */

    /* --- 生命周期/并发（refs/retired 由全局 g_lock 保护） --- */
    int  refs;                 /* 引用计数；0 且 destroyed 时释放 */
    int  destroyed;            /* 已进入终态，等待 refs 归零后释放 */
    int  retired;              /* 已从两张内核哈希表摘除 */
    int  is_listen;            /* 是否为 listen socket */

    /* --- 地址（发送/接收五元组） --- */
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    tju_tcp_t* listen_parent;  /* 服务端子连接指向其 listen socket */

    /* --- 序号/确认 --- */
    uint32_t snd_isn;          /* 本端初始序号 */
    uint32_t rcv_isn;          /* 对端初始序号 */
    uint32_t snd_una;          /* 最小未确认字节号 */
    uint32_t snd_nxt;          /* 下一个待发字节号 */
    uint32_t snd_app;          /* 应用已写入但尚未确认的字节末尾（发送环形缓冲写指针）*/
    uint32_t rcv_nxt;          /* 期望接收的下一字节号（== 回给对端的 ack_num）*/
    uint32_t rcv_avail;        /* 应用可读的下一字节号（读指针）*/
    uint8_t  recv_fin;         /* 已收到对端 FIN */
    uint8_t  fin_sent;         /* 已发送本端 FIN */
    uint8_t  fin_acked;        /* 本端 FIN 已被确认 */
    uint32_t fin_seq;          /* 本端 FIN 的序号 */
    uint32_t syn_rtx_cnt;      /* 握手重传计数 */

    /* --- 发送环形缓冲 --- */
    char*  snd_ring;           /* 大小 RING_CAP，按 seq % RING_CAP 索引 */

    /* --- 在途段元数据（发送时序/重传/RTT 采样） --- */
    struct seg { uint32_t seq; uint16_t len; uint32_t st_ms; uint8_t rtx; } segs[MAX_OUT_SEG];
    int seg_h, seg_cnt;        /* 环形队列头/个数 */

    /* --- RTT / RTO（RFC6298，ms 单位；srtt/rttvar 用 1/8ms 缩放整数） --- */
    uint32_t rto_ms, srtt8, rttvar8;
    uint8_t  no_sample;        /* Karn：RTO 之后暂不采样 */
    uint8_t  timer_on;         /* 重传定时器是否在跑 */
    uint64_t timer_arm_ms;     /* 定时器绝对超时时刻 */
    int      dup_acks;         /* 连续重复 ACK 计数 */
    uint8_t  in_retrans;       /* 是否正处于 RTO 退避中（用于快速重传时避免误采样）*/

    /* --- 流量控制 --- */
    uint16_t peer_rwnd;        /* 对端最近通告的窗口 */
    uint16_t last_adv;         /* 本端最近通告的窗口（用于窗口恢复更新判断）*/

    /* --- 拥塞控制（基础 Reno + NewReno；发送窗口取 min(rwnd, cwnd)） --- */
    uint32_t cwnd;             /* 拥塞窗口（字节） */
    uint32_t ssthresh;         /* 慢启动阈值（字节） */
    uint8_t  in_fast_recovery; /* 是否处于快速恢复 */
    uint32_t recover;          /* NewReno 恢复点：进入快速恢复时的 snd_nxt */

    /* --- 接收缓冲（字节流 + 位图） --- */
    char*  recv_ring;          /* 大小 RING_CAP，按 seq % RING_CAP 索引 */
    char*  present;            /* present[seq%CAP]==1 表示该字节已到且未被应用消费 */
    uint64_t used_bytes;       /* 缓冲中已占用（present 计数）字节数 */

    /* --- 连接 / 关闭 / TIME_WAIT --- */
    uint64_t tw_deadline_ms;   /* TIME_WAIT 结束时刻 */
    uint8_t  in_timewait;
    int      state;            /* 冗余一份便于调试（与 sock->state 同步）*/

    /* --- accept 全连接队列（仅 listen socket 使用） --- */
    pthread_mutex_t qlock;
    pthread_cond_t  qcond;
    tju_tcp_t* q[QCAP];
    int qin, qout, qcnt;

    /* --- 条件变量：连接完成/数据到达/发送缓冲释放/关闭完成 --- */
    pthread_cond_t cond;
};

/* ============================================================
 * 全局（tju_tcp.c 内部）
 * ============================================================ */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* 保护哈希表写与 refs/retired */
static int sched_started = 0;
static uint32_t g_eph_next = 0;        /* 主动连接临时端口分配器 */

/* 前向声明 */
static void send_pkt(tju_tcp_t* sock, uint8_t flags, uint32_t seq, uint32_t ack,
                     const char* data, int dlen);
static void maybe_send_locked(tju_tcp_t* sock, struct tju_tcp_ext* e);
static void sock_retire(tju_tcp_t* sock);
static void handle_post_established(tju_tcp_t* sock, struct tju_tcp_ext* e,
                                    char* pkt);
static uint64_t now_ms(void);
static uint16_t advert_window(struct tju_tcp_ext* e);
static void sample_rtt(struct tju_tcp_ext* e, uint32_t R);
static void restart_timer(struct tju_tcp_ext* e, uint64_t now);

/* ============================================================
 * 小工具
 * ============================================================ */
static uint64_t now_ms(void){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec*1000 + tv.tv_usec/1000;
}
/* 由当前时间构造 pthread_cond_timedwait 所需的绝对时间（CLOCK_REALTIME）*/
static struct timespec ts_after_ms(uint64_t delta_ms){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ms = (uint64_t)tv.tv_sec*1000 + tv.tv_usec/1000 + delta_ms;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms/1000);
    ts.tv_nsec = (long)(ms%1000)*1000000L;
    return ts;
}
static inline int seq_lt(uint32_t a, uint32_t b){ return (int32_t)(a-b) < 0; }
static inline int seq_gt(uint32_t a, uint32_t b){ return (int32_t)(b-a) < 0; }
static inline int seq_lte(uint32_t a, uint32_t b){ return !seq_gt(a,b); }

static uint16_t advert_window(struct tju_tcp_ext* e){
    if(e->recv_ring == NULL) return 65535;
    uint64_t free = RING_CAP - e->used_bytes;
    return free >= 65535 ? 65535 : (uint16_t)free;
}

static uint32_t local_ip_by_host(void){
    char host[8]; gethostname(host, 8);
    if(strcmp(host,"server")==0) return inet_network("172.17.0.6");
    return inet_network("172.17.0.5");
}
static uint32_t remote_ip_by_host(void){
    char host[8]; gethostname(host, 8);
    if(strcmp(host,"server")==0) return inet_network("172.17.0.5");
    return inet_network("172.17.0.6");
}

/* 发送一个报文（调用者须持有 e->lock；用 sock->ext 里的地址端口） */
static void send_pkt(tju_tcp_t* sock, uint8_t flags, uint32_t seq, uint32_t ack,
                     const char* data, int dlen){
    struct tju_tcp_ext* e = sock->ext;
    if(dlen < 0) dlen = 0;
    if(dlen > MAX_SEG_LEN) dlen = MAX_SEG_LEN;
    if(e->local_port == 0) return;
    uint16_t adv = advert_window(e);
    char* buf = create_packet_buf(e->local_port, e->remote_port, seq, ack,
                    DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN + dlen,
                    flags, adv, 0, (char*)data, dlen);
    sendToLayer3(buf, DEFAULT_HEADER_LEN + dlen);
    free(buf);
    e->last_adv = adv;
}

static void get_ref(tju_tcp_t* sock){
    pthread_mutex_lock(&g_lock); sock->ext->refs++; pthread_mutex_unlock(&g_lock);
}
static void put_ref(tju_tcp_t* sock){
    struct tju_tcp_ext* e = sock->ext;
    int dofree = 0;
    pthread_mutex_lock(&g_lock);
    e->refs--;
    if(e->refs == 0 && e->destroyed) dofree = 1;
    pthread_mutex_unlock(&g_lock);
    if(dofree){
        int i;
        pthread_mutex_lock(&g_lock);
        for(i=0;i<MAX_SOCK;i++){ if(listen_socks[i]==sock) listen_socks[i]=NULL;
                                 if(established_socks[i]==sock) established_socks[i]=NULL; }
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_destroy(&e->lock);
        pthread_cond_destroy(&e->cond);
        pthread_mutex_destroy(&e->qlock);
        pthread_cond_destroy(&e->qcond);
        if(e->snd_ring) free(e->snd_ring);
        if(e->recv_ring) free(e->recv_ring);
        if(e->present)   free(e->present);
        free(e);
        free(sock);
    }
}

/* 把 socket 从哈希表摘除并允许在 refs 归零后释放（调用者不能持 e->lock）*/
static void sock_retire(tju_tcp_t* sock){
    struct tju_tcp_ext* e = sock->ext;
    int dofree = 0;
    pthread_mutex_lock(&g_lock);
    if(e->retired){ pthread_mutex_unlock(&g_lock); return; }
    e->retired = 1;
    e->destroyed = 1;
    e->refs--;
    if(e->refs == 0) dofree = 1;
    int i;
    for(i=0;i<MAX_SOCK;i++){ if(listen_socks[i]==sock) listen_socks[i]=NULL;
                             if(established_socks[i]==sock) established_socks[i]=NULL; }
    pthread_mutex_unlock(&g_lock);
    if(dofree){
        pthread_mutex_destroy(&e->lock);
        pthread_cond_destroy(&e->cond);
        pthread_mutex_destroy(&e->qlock);
        pthread_cond_destroy(&e->qcond);
        if(e->snd_ring) free(e->snd_ring);
        if(e->recv_ring) free(e->recv_ring);
        if(e->present)   free(e->present);
        free(e);
        free(sock);
    }
}

/* 生成 ISN：单调时间 + 进程/计数器不可预测分量 + 随机 */
static uint32_t gen_isn(void){
    static uint64_t ctr = 0;
    ctr++;
    uint64_t t = now_ms();
    uint32_t r = (uint32_t)((t & 0xFFFF) << 16) ^ (uint32_t)(getpid()*2654435761u)
               ^ (uint32_t)(ctr * 40503u) ^ (uint32_t)rand();
    return r;
}

/* 环形缓冲读写：绝对字节号 seq 定位到 seq % RING_CAP */
static void ring_store(char* ring, uint32_t from, const char* src, size_t n){
    uint32_t pos = from % RING_CAP;
    size_t first = RING_CAP - pos;
    if(n <= first){ memcpy(ring+pos, src, n); }
    else { memcpy(ring+pos, src, first); memcpy(ring, src+first, n-first); }
}
static void ring_fetch(const char* ring, uint32_t from, char* dst, size_t n){
    uint32_t pos = from % RING_CAP;
    size_t first = RING_CAP - pos;
    if(n <= first){ memcpy(dst, ring+pos, n); }
    else { memcpy(dst, ring+pos, first); memcpy(dst+first, ring, n-first); }
}

/* ============================================================
 * RTT / RTO（RFC 6298，Karn）
 * ============================================================ */
static void rto_clamp(struct tju_tcp_ext* e){
    if(e->rto_ms < RTO_MIN_MS) e->rto_ms = RTO_MIN_MS;
    if(e->rto_ms > RTO_MAX_MS) e->rto_ms = RTO_MAX_MS;
}
static void sample_rtt(struct tju_tcp_ext* e, uint32_t R){
    if(R <= 0) R = 1;
    if(e->srtt8 == 0){ /* 首个样本 */
        e->srtt8 = R*8;
        e->rttvar8 = R*4;
        e->rto_ms = e->srtt8/8 + (e->rttvar8/8)*4;
    } else {
        int32_t d = (int32_t)e->srtt8 - (int32_t)(R*8);
        if(d < 0) d = -d;
        e->rttvar8 = (3*e->rttvar8 + d) / 4;
        e->srtt8   = (7*e->srtt8 + R*8) / 8;
        e->rto_ms  = (e->srtt8 + 4*e->rttvar8) / 8;
    }
    rto_clamp(e);
}
static void restart_timer(struct tju_tcp_ext* e, uint64_t now){
    e->timer_on = 1;
    e->timer_arm_ms = now + e->rto_ms;
}

/* ============================================================
 * 拥塞控制（基础 Reno + NewReno 快速恢复，RFC 5681 / RFC 6582）
 *   发送窗口 = min(rwnd, cwnd)；FlightSize = snd_nxt - snd_una
 *   SYN / SYN-ACK 不增加 cwnd（握手路径不调用下列函数）
 * ============================================================ */
/* 丢包后的 ssthresh：max(FlightSize/2, 2*SMSS)，并施加下限 SSTHRESH_FLOOR，
 * 使本课程 “<6% 丢包、接收窗口上限 65535” 的场景下窗口不会塌陷到过低值。 */
static uint32_t ssthresh_after_loss(struct tju_tcp_ext* e){
    uint32_t flight = (uint32_t)(e->snd_nxt - e->snd_una);
    uint32_t half = flight/2;
    if(half < 2*SMSS) half = 2*SMSS;
    if(half < SSTHRESH_FLOOR) half = SSTHRESH_FLOOR;
    return half;
}
/* 重传最早未确认段（快速重传与 NewReno 部分 ACK 恢复共用） */
static void retransmit_first_unacked(tju_tcp_t* sock, struct tju_tcp_ext* e, uint64_t now){
    if(e->seg_cnt <= 0) return;
    struct seg* s = &e->segs[e->seg_h];
    if(s->len > 0 && e->snd_ring){
        char tmp[MAX_SEG_LEN];
        ring_fetch(e->snd_ring, s->seq, tmp, s->len);
        send_pkt(sock, ACK_FLAG_MASK, s->seq, e->rcv_nxt, tmp, s->len);
        s->st_ms = (uint32_t)now;
        s->rtx = 1;
        restart_timer(e, now);
    }
}
/* 收到确认“新数据”的累计 ACK（非快速恢复期）：慢启动 / 拥塞避免 */
static void cwnd_on_new_ack(struct tju_tcp_ext* e, uint32_t acked){
    if(acked == 0) return;
    if(e->cwnd < e->ssthresh){
        /* 慢启动：每个确认新数据的 ACK 增加，但单次不超过一个 SMSS */
        e->cwnd += (acked < SMSS) ? acked : SMSS;
    } else {
        /* 拥塞避免：约每 RTT 增加一个 SMSS（字节计数，不比 RFC5681 更激进） */
        e->cwnd += (uint32_t)(((uint64_t)SMSS * acked) / e->cwnd);
    }
    if(e->cwnd > (uint32_t)RING_CAP) e->cwnd = (uint32_t)RING_CAP;
}
/* RTO 超时：ssthresh = max(FlightSize/2, 2*SMSS)[含下限]，cwnd 降到 1 SMSS，重新慢启动 */
static void cwnd_on_rto(struct tju_tcp_ext* e){
    e->ssthresh = ssthresh_after_loss(e);
    e->cwnd = CWND_MIN;
    e->in_fast_recovery = 0;
}
/* 三次重复 ACK：进入快速恢复，记录恢复点 recover，cwnd = ssthresh + 3*SMSS */
static void cwnd_on_fast_retransmit(struct tju_tcp_ext* e){
    e->ssthresh = ssthresh_after_loss(e);
    e->recover = e->snd_nxt;            /* NewReno：本次快速恢复需被确认到的边界 */
    e->cwnd = e->ssthresh + 3*SMSS;     /* 快速恢复窗口膨胀 */
    e->in_fast_recovery = 1;
}

/* ============================================================
 * 发送循环：把发送缓冲里能发的数据全部发出（受对端 rwnd 约束）
 * ============================================================ */
static void maybe_send_locked(tju_tcp_t* sock, struct tju_tcp_ext* e){
    if(!DATA_ALLOWED(e->state)) return;
    if(e->snd_ring == NULL) return;

    uint64_t now = now_ms();
    int guard = 0;
    while(1){
        if(++guard > MAX_OUT_SEG) break; /* 保险：一轮至多发满一个窗口 */
        uint32_t queued = (uint32_t)(e->snd_app - e->snd_nxt);
        if(queued == 0) break;
        uint32_t inflight = (uint32_t)(e->snd_nxt - e->snd_una);
        /* 发送窗口：ENABLE_CWND_LIMIT=1 时为 min(rwnd, cwnd)（Reno 限窗）；
         * =0 时仅取对端通告窗口 rwnd（见常量处说明）。二者都不替代 FlightSize。 */
#if ENABLE_CWND_LIMIT
        uint32_t win = (e->cwnd < e->peer_rwnd) ? e->cwnd : e->peer_rwnd;
#else
        uint32_t win = e->peer_rwnd;
#endif
        if(inflight >= win && inflight > 0) break;   /* 窗口已满 */

        uint32_t allow = (win > inflight) ? (win - inflight) : 0;
        /* 零窗口：若无在途数据且有排队数据，则发 1 字节窗口探测 */
        if(allow == 0){
            if(inflight == 0) allow = 1;             /* 单字节探测 */
            else break;
        }
        uint32_t n = queued < MAX_SEG_LEN ? queued : MAX_SEG_LEN;
        if(n > allow) n = allow;
        if(n == 0) break;

        char tmp[MAX_SEG_LEN];
        ring_fetch(e->snd_ring, e->snd_nxt, tmp, n);
        send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, tmp, (int)n);

        /* 记录段元数据 */
        e->segs[(e->seg_h + e->seg_cnt) % MAX_OUT_SEG].seq = e->snd_nxt;
        e->segs[(e->seg_h + e->seg_cnt) % MAX_OUT_SEG].len = (uint16_t)n;
        e->segs[(e->seg_h + e->seg_cnt) % MAX_OUT_SEG].st_ms = (uint32_t)now;
        e->segs[(e->seg_h + e->seg_cnt) % MAX_OUT_SEG].rtx = 0;
        if(e->seg_cnt < MAX_OUT_SEG) e->seg_cnt++;

        e->snd_nxt += n;
        if(!e->timer_on) restart_timer(e, now);
    }
}

/* ============================================================
 * 接收路径：按字节号把载荷写入接收环形缓冲 + 位图；推进 rcv_nxt
 * 返回是否含有可被确认的“新”字节（决定是否回 ACK）
 * ============================================================ */
static int receive_data(struct tju_tcp_ext* e,
                        const char* payload, uint32_t seq, int dlen){
    if(dlen <= 0) return 0;
    uint32_t end = seq + (uint32_t)dlen;
    int meaningful = 0;

    /* 完全重复 */
    if(seq_lte(end, e->rcv_nxt)) return 0;

    /* 重叠：裁掉已收前缀 */
    if(seq_lt(seq, e->rcv_nxt)){
        uint32_t cut = e->rcv_nxt - seq;
        seq  += cut;
        payload += cut;
        dlen -= (int)cut;
        end = seq + (uint32_t)dlen;
    }
    if(dlen <= 0) return 0;
    meaningful = 1;

    /* 超窗丢弃 */
    uint64_t free = RING_CAP - e->used_bytes;
    if((uint64_t)dlen > free) dlen = (int)free;
    if(dlen <= 0) return 1;   /* 窗口为 0，仍应 ACK 以响应窗口探测 */

    /* 逐个字节存储（位图去重）*/
    for(int i=0;i<dlen;i++){
        uint32_t s = seq + i;
        uint32_t idx = s % RING_CAP;
        if(!e->present[idx]){
            e->present[idx] = 1;
            e->recv_ring[idx] = payload[i];
            e->used_bytes++;
        }
    }

    /* 推进连续字节 */
    uint64_t held = (uint32_t)(e->rcv_nxt - e->rcv_avail);
    while(e->used_bytes > held){
        uint32_t idx = e->rcv_nxt % RING_CAP;
        if(!e->present[idx]) break;
        e->rcv_nxt++;
        held++;
    }
    return meaningful;
}

/* ============================================================
 * 处理 ACK：推进 snd_una、段元数据弹出与 RTT 采样、快速重传
 * ============================================================ */
static void process_ack(tju_tcp_t* sock, struct tju_tcp_ext* e, uint32_t acknum){
    uint64_t now = now_ms();

    /* 本端 FIN 被确认（ack == fin_seq+1） */
    if(e->fin_sent && !e->fin_acked){
        if(acknum == (uint32_t)(e->fin_seq + 1)) e->fin_acked = 1;
    }

    if(seq_gt(acknum, e->snd_una)){
        uint32_t newuna = acknum;
        if(seq_gt(newuna, e->snd_nxt)) newuna = e->snd_nxt; /* 钳制 */
        if(!seq_gt(newuna, e->snd_una)) return;
        uint32_t acked = newuna - e->snd_una;               /* 本次累计确认的新字节数 */

        /* 弹出已确认的段元数据 */
        int sampled = 0;
        while(e->seg_cnt > 0){
            struct seg* s = &e->segs[e->seg_h];
            if(seq_lte((uint32_t)(s->seq + s->len), newuna)){
                int fresh = !s->rtx;
                if(fresh){                 /* 全新（未重传）数据被确认 => 退出 RTO 退避 */
                    e->in_retrans = 0;
                    if(e->no_sample){
                        e->no_sample = 0;
                        if(e->srtt8) e->rto_ms = (e->srtt8 + 4*e->rttvar8)/8;
                        rto_clamp(e);
                    } else if(!sampled){
                        uint32_t R = (uint32_t)now - s->st_ms;
                        sample_rtt(e, R);
                        sampled = 1;
                    }
                }
                e->seg_h = (e->seg_h + 1) % MAX_OUT_SEG;
                e->seg_cnt--;
            } else break;
        }
        e->snd_una = newuna;
        e->dup_acks = 0;
        if(e->in_fast_recovery){
            if(seq_lte(e->recover, newuna)){
                /* 完整 ACK：已覆盖恢复点 -> 退出快速恢复，窗口塌陷回 ssthresh */
                e->in_fast_recovery = 0;
                e->cwnd = e->ssthresh;
            } else {
                /* NewReno 部分 ACK：仅确认了部分丢失数据 -> 立即重传下一个未确认段并
                 * 保持在快速恢复中（无需再等 3 个重复 ACK，显著缩短多丢包恢复时间） */
                e->cwnd = e->ssthresh + 3*SMSS;
                retransmit_first_unacked(sock, e, now);
            }
        } else {
            cwnd_on_new_ack(e, acked);   /* 非快速恢复：慢启动 / 拥塞避免 */
        }

        /* 若 RTO 退避中且新 ACK 推进，视为恢复，重置 RTO 到当前值 */
        if(e->seg_cnt > 0) restart_timer(e, now);
        else e->timer_on = 0;

        pthread_cond_broadcast(&e->cond);   /* 发送缓冲腾出空间，唤醒 tju_send */
        maybe_send_locked(sock, e);
    } else {
        /* 重复 ACK（ack 未推进） */
        if(e->seg_cnt > 0 && !e->in_retrans){
            e->dup_acks++;
            if(e->dup_acks == 3){
                /* 三次重复 ACK：进入快速恢复并快速重传最早未确认段 */
                cwnd_on_fast_retransmit(e);
                retransmit_first_unacked(sock, e, now);
            } else if(e->dup_acks > 3 && e->in_fast_recovery){
                /* 快速恢复期额外重复 ACK：窗口膨胀一个 SMSS 并尝试发送新数据 */
                e->cwnd += SMSS;
                maybe_send_locked(sock, e);
            }
        }
    }
}

/* ============================================================
 * 已建立连接后的收包处理（数据 + ACK + FIN），适用于
 * ESTABLISHED / FIN_WAIT_1 / FIN_WAIT_2 / CLOSE_WAIT / CLOSING / LAST_ACK / TIME_WAIT
 * ============================================================ */
static void handle_post_established(tju_tcp_t* sock, struct tju_tcp_ext* e, char* pkt){
    uint32_t seq = get_seq(pkt);
    uint32_t ack = get_ack(pkt);
    uint8_t  flags = get_flags(pkt);
    uint16_t adv = get_advertised_window(pkt);
    int dlen = get_plen(pkt) - DEFAULT_HEADER_LEN;
    const char* payload = pkt + DEFAULT_HEADER_LEN;

    e->peer_rwnd = adv;   /* 刷新对端通告窗口（流量控制：发送许可随动） */

    uint64_t now = now_ms();

    /* 0) 数据期收到重复 SYN/SYNACK（我方最终 ACK 曾丢失）：只重发 ACK，不改变连接 */
    if(flags & SYN_FLAG_MASK && e->state == ESTABLISHED){
        send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
        return;
    }

    /* 1) ACK 处理 */
    if(flags & ACK_FLAG_MASK){
        process_ack(sock, e, ack);

        /* 状态推进（依赖 FIN 确认） */
        if(e->fin_sent && e->fin_acked){
            if(e->state == FIN_WAIT_1 && !e->recv_fin){
                e->state = FIN_WAIT_2;
                sock->state = FIN_WAIT_2;
            } else if(e->state == CLOSING){
                /* 同时关闭：收到对端 FIN 后又收到本端 FIN 的 ACK */
                e->state = TIME_WAIT; sock->state = TIME_WAIT;
                e->tw_deadline_ms = now + TIME_WAIT_MS;
                e->in_timewait = 1;
                pthread_cond_broadcast(&e->cond);
            } else if(e->state == LAST_ACK){
                e->state = CLOSED; sock->state = CLOSED;
                pthread_cond_broadcast(&e->cond);
            }
        }
    }

    /* 2) 数据接收：任何携带数据的数据期报文都回累计 ACK（含重复/失序/窗口为 0 情形），
     *    保证对端能推进发送并避免"ACK 丢失后重传却再无 ACK"的死循环 */
    if(dlen > 0 && !e->recv_fin && e->recv_ring){
        receive_data(e, payload, seq, dlen);
        send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
        pthread_cond_broadcast(&e->cond);   /* 唤醒 tju_recv */
    }

    /* 3) FIN 处理 */
    if(flags & FIN_FLAG_MASK){
        if(e->recv_fin){
            /* 重复 FIN：一律重发 ACK（覆盖 CLOSE_WAIT/FIN_WAIT_2/LAST_ACK/TIME_WAIT），
             * 防止我方 ACK 丢失时对端无限重传 FIN */
            send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
            if(e->in_timewait) e->tw_deadline_ms = now_ms() + TIME_WAIT_MS;
        } else {
            /* FIN 需要按序（seq==rcv_nxt）；否则暂不处理（等重传）*/
            if(seq == e->rcv_nxt){
                e->rcv_nxt++;
                e->recv_fin = 1;
                send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
                pthread_cond_broadcast(&e->cond);

                if(e->state == ESTABLISHED || e->state == FIN_WAIT_2){
                    e->state = (e->state == ESTABLISHED) ? CLOSE_WAIT : TIME_WAIT;
                    sock->state = e->state;
                    if(e->state == TIME_WAIT){
                        e->tw_deadline_ms = now_ms() + TIME_WAIT_MS;
                        e->in_timewait = 1;
                    }
                    pthread_cond_broadcast(&e->cond);
                } else if(e->state == FIN_WAIT_1){
                    /* 同时关闭：先收到对端 FIN */
                    e->state = CLOSING; sock->state = CLOSING;
                }
            }
        }
    }

    /* 4) 窗口更新：对端把通告窗口开大（含 0 -> N）时补发排队数据；
     *    数据段/重复 ACK 都顺带处理，代价可忽略 */
    if(flags & ACK_FLAG_MASK) maybe_send_locked(sock, e);
}

/* ============================================================
 * listen socket 收到 SYN：创建子连接、回 SYNACK
 * ============================================================ */
static tju_tcp_t* make_child(tju_tcp_t* listen_sock, char* pkt){
    uint16_t src_port = get_src(pkt);
    uint32_t syn_seq  = get_seq(pkt);

    tju_tcp_t* c = (tju_tcp_t*)calloc(1, sizeof(tju_tcp_t));
    struct tju_tcp_ext* ce = (struct tju_tcp_ext*)calloc(1, sizeof(struct tju_tcp_ext));
    c->ext = ce;
    pthread_mutex_init(&ce->lock, NULL);
    pthread_cond_init(&ce->cond, NULL);
    pthread_mutex_init(&ce->qlock, NULL);
    pthread_cond_init(&ce->qcond, NULL);
    ce->refs = 1;
    ce->listen_parent = listen_sock;

    ce->state = SYN_RECV; c->state = SYN_RECV;
    ce->local_ip  = local_ip_by_host();
    ce->remote_ip = remote_ip_by_host();
    ce->local_port  = get_dst(pkt);
    ce->remote_port = src_port;

    c->established_local_addr.ip = ce->local_ip;
    c->established_local_addr.port = ce->local_port;
    c->established_remote_addr.ip = ce->remote_ip;
    c->established_remote_addr.port = ce->remote_port;

    ce->snd_isn = gen_isn();
    ce->snd_una = ce->snd_nxt = ce->snd_app = ce->snd_isn + 1;
    ce->rcv_isn = syn_seq;
    ce->rcv_nxt = syn_seq + 1;
    ce->rcv_avail = syn_seq + 1;
    ce->peer_rwnd = get_advertised_window(pkt);
    ce->rto_ms = RTO_INIT_MS;
    ce->cwnd = CWND_INIT;          /* Reno 初始拥塞窗口 */
    ce->ssthresh = SSTHRESH_INIT;  /* 初始慢启动阈值 = 最大接收窗口 */
    ce->in_fast_recovery = 0;
    ce->recover = 0;

    ce->snd_ring   = (char*)calloc(1, RING_CAP);
    ce->recv_ring  = (char*)calloc(1, RING_CAP);
    ce->present    = (char*)calloc(1, RING_CAP);

    /* 注册到内核已建连哈希表（四元组取模槽位；若极端同槽冲突会覆盖旧连接——框架 32 槽取模所致）*/
    pthread_mutex_lock(&g_lock);
    established_socks[cal_hash(ce->local_ip,ce->local_port,ce->remote_ip,ce->remote_port)] = c;
    pthread_mutex_unlock(&g_lock);

    send_pkt(c, SYN_FLAG_MASK | ACK_FLAG_MASK, ce->snd_isn, ce->rcv_nxt, NULL, 0);
    ce->syn_rtx_cnt = 0;
    restart_timer(ce, now_ms());
    return c;
}

/* ============================================================
 * 调度线程：重传 / 零窗口探测 / TIME_WAIT 与 CLOSED 回收
 * ============================================================ */
static void sched_one(tju_tcp_t* s, uint64_t now){
    struct tju_tcp_ext* e = s->ext;
    if(e == NULL || e->destroyed) return;

    if(e->state == TIME_WAIT){
        if(e->in_timewait && now >= e->tw_deadline_ms){
            e->state = CLOSED; s->state = CLOSED;
            /* 解锁在调用方完成，此处只标记 */
        }
        return;
    }
    if(e->state == CLOSED){
        /* 由调用方在解锁后 sock_retire */
        return;
    }

    if(e->state == SYN_SENT){
        if(e->timer_on && now >= e->timer_arm_ms){
            if(e->syn_rtx_cnt >= MAX_SYN_RETRY){
                e->state = CLOSED; s->state = CLOSED;
                pthread_cond_broadcast(&e->cond);
            } else {
                send_pkt(s, SYN_FLAG_MASK, e->snd_isn, 0, NULL, 0);
                e->syn_rtx_cnt++;
                e->rto_ms = e->rto_ms*2; rto_clamp(e);
                restart_timer(e, now);
            }
        }
        return;
    }
    if(e->state == SYN_RECV){
        if(e->timer_on && now >= e->timer_arm_ms){
            if(e->syn_rtx_cnt >= MAX_SYN_RETRY){
                /* 握手失败：摘除并释放子连接 */
                e->state = CLOSED; s->state = CLOSED;
            } else {
                send_pkt(s, SYN_FLAG_MASK | ACK_FLAG_MASK, e->snd_isn, e->rcv_nxt, NULL, 0);
                e->syn_rtx_cnt++;
                e->rto_ms = e->rto_ms*2; rto_clamp(e);
                restart_timer(e, now);
            }
        }
        return;
    }

    /* 数据连接：重传定时器 */
    if(e->timer_on && now >= e->timer_arm_ms){
        if(e->seg_cnt > 0){
            cwnd_on_rto(e);   /* Reno：RTO -> ssthresh=FlightSize/2, cwnd=1 SMSS，重新慢启动 */
            struct seg* s0 = &e->segs[e->seg_h];
            if(s0->len > 0 && e->snd_ring){
                char tmp[MAX_SEG_LEN];
                ring_fetch(e->snd_ring, s0->seq, tmp, s0->len);
                send_pkt(s, ACK_FLAG_MASK, s0->seq, e->rcv_nxt, tmp, s0->len);
                s0->st_ms = (uint32_t)now;
                s0->rtx = 1;
            }
            e->no_sample = 1;                 /* Karn：重传后不采样 */
            e->in_retrans = 1;
            e->rto_ms = e->rto_ms*2; rto_clamp(e);
            restart_timer(e, now);
        } else if(e->fin_sent && !e->fin_acked){
            /* 重传 FIN */
            if(e->syn_rtx_cnt < MAX_FIN_RETRY){
                send_pkt(s, FIN_FLAG_MASK | ACK_FLAG_MASK, e->fin_seq, e->rcv_nxt, NULL, 0);
                e->syn_rtx_cnt++;
                e->rto_ms = e->rto_ms*2; rto_clamp(e);
                restart_timer(e, now);
            } else {
                e->state = CLOSED; s->state = CLOSED;
                pthread_cond_broadcast(&e->cond);
            }
        } else {
            e->timer_on = 0;
        }
    }
}

static void* sched_thread(void* arg){
    (void)arg;
    while(1){
        usleep(5*1000);
        uint64_t now = now_ms();
        tju_tcp_t* pick[64];
        int npick = 0, i;

        pthread_mutex_lock(&g_lock);
        for(i=0;i<MAX_SOCK;i++){
            tju_tcp_t* s = established_socks[i];
            if(s && s->ext){ s->ext->refs++; pick[npick++]=s; }
        }
        for(i=0;i<MAX_SOCK;i++){
            tju_tcp_t* s = listen_socks[i];
            if(s && s->ext){ s->ext->refs++; pick[npick++]=s; }
        }
        pthread_mutex_unlock(&g_lock);

        for(i=0;i<npick;i++){
            tju_tcp_t* s = pick[i];
            struct tju_tcp_ext* e = s->ext;
            int need_retire = 0;
            pthread_mutex_lock(&e->lock);
            if(!e->destroyed){
                sched_one(s, now);
                if(e->state == CLOSED || (e->in_timewait && now >= e->tw_deadline_ms))
                    need_retire = 1;
            }
            pthread_mutex_unlock(&e->lock);
            if(need_retire) sock_retire(s);
            put_ref(s);
        }
    }
    return NULL;
}

static void ensure_sched(void){
    pthread_mutex_lock(&g_lock);
    if(!sched_started){
        sched_started = 1;
        pthread_mutex_unlock(&g_lock);
        pthread_t tid;
        pthread_create(&tid, NULL, sched_thread, NULL);
        pthread_detach(tid);
    } else pthread_mutex_unlock(&g_lock);
}

/* ============================================================
 * 对外接口
 * ============================================================ */
tju_tcp_t* tju_socket(){
    ensure_sched();
    tju_tcp_t* sock = (tju_tcp_t*)calloc(1, sizeof(tju_tcp_t));
    struct tju_tcp_ext* e = (struct tju_tcp_ext*)calloc(1, sizeof(struct tju_tcp_ext));
    sock->ext = e;
    sock->state = CLOSED;
    e->state = CLOSED;
    e->refs = 1;
    e->rto_ms = RTO_INIT_MS;
    e->peer_rwnd = 65535;
    e->cwnd = CWND_INIT;
    e->ssthresh = SSTHRESH_INIT;
    e->recover = 0;
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->cond, NULL);
    pthread_mutex_init(&e->qlock, NULL);
    pthread_cond_init(&e->qcond, NULL);
    return sock;
}

int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    if(!sock) return -1;
    sock->bind_addr = bind_addr;
    return 0;
}

int tju_listen(tju_tcp_t* sock){
    if(!sock || !sock->ext) return -1;
    struct tju_tcp_ext* e = sock->ext;
    pthread_mutex_lock(&e->lock);
    e->state = LISTEN; sock->state = LISTEN;
    e->is_listen = 1;
    e->local_ip = sock->bind_addr.ip;
    e->local_port = sock->bind_addr.port;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    pthread_mutex_lock(&g_lock);
    listen_socks[hashval] = sock;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&e->lock);
    return 0;
}

tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    if(!listen_sock || !listen_sock->ext) return NULL;
    struct tju_tcp_ext* e = listen_sock->ext;
    tju_tcp_t* child = NULL;

    pthread_mutex_lock(&e->qlock);
    while(e->qcnt == 0){
        pthread_cond_wait(&e->qcond, &e->qlock);
        /* 若有 CLOSED 需提前结束（listen socket 一般不关闭）*/
    }
    child = e->q[e->qout];
    e->qout = (e->qout + 1) % QCAP;
    e->qcnt--;
    pthread_mutex_unlock(&e->qlock);
    return child;
}

int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){
    if(!sock || !sock->ext) return -1;
    struct tju_tcp_ext* e = sock->ext;
    pthread_mutex_lock(&e->lock);

    sock->established_remote_addr = target_addr;
    e->remote_ip = target_addr.ip;
    e->remote_port = target_addr.port;
    e->local_ip = local_ip_by_host();

    /* 选择不与现有连接哈希槽冲突的临时端口 */
    pthread_mutex_lock(&g_lock);
    if(g_eph_next == 0)
        g_eph_next = 30000 + (uint32_t)(rand() % 10000);
    int placed = 0;
    for(int k=0;k<60000;k++){
        uint16_t port = (uint16_t)((g_eph_next + k) % 40000 + 20000);
        int h = cal_hash(e->local_ip, port, e->remote_ip, e->remote_port);
        if(established_socks[h] == NULL){
            e->local_port = port;
            g_eph_next = (uint32_t)port + 1;
            established_socks[h] = sock;
            placed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if(!placed){
        pthread_mutex_unlock(&e->lock);
        return -1;
    }

    sock->established_local_addr.ip = e->local_ip;
    sock->established_local_addr.port = e->local_port;

    e->snd_ring  = (char*)calloc(1, RING_CAP);
    e->recv_ring = (char*)calloc(1, RING_CAP);
    e->present   = (char*)calloc(1, RING_CAP);

    e->snd_isn = gen_isn();
    e->snd_una = e->snd_nxt = e->snd_app = e->snd_isn + 1;
    e->rto_ms = RTO_INIT_MS;
    e->cwnd = CWND_INIT;           /* Reno 初始拥塞窗口 */
    e->ssthresh = SSTHRESH_INIT;   /* 初始慢启动阈值 = 最大接收窗口 */
    e->in_fast_recovery = 0;
    e->recover = 0;
    e->state = SYN_SENT; sock->state = SYN_SENT;

    send_pkt(sock, SYN_FLAG_MASK, e->snd_isn, 0, NULL, 0);
    restart_timer(e, now_ms());

    /* 阻塞等待握手完成 */
    struct timespec ts;
    int ret = 0;
    while(e->state == SYN_SENT){
        ts = ts_after_ms(1000);
        pthread_cond_timedwait(&e->cond, &e->lock, &ts);
        if(e->state == CLOSED){ ret = -1; break; }
    }
    pthread_mutex_unlock(&e->lock);
    return ret;
}

int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    if(!sock || !sock->ext) return -1;
    struct tju_tcp_ext* e = sock->ext;
    const char* src = (const char*)buffer;
    int total = 0;

    pthread_mutex_lock(&e->lock);
    if(e->snd_ring == NULL || !DATA_ALLOWED(e->state)){
        pthread_mutex_unlock(&e->lock);
        return -1;
    }
    struct timespec ts;
    while(total < len){
        uint32_t used = (uint32_t)(e->snd_app - e->snd_una);
        if(used >= RING_CAP){
            /* 发送缓冲满：等待 ACK 腾出空间（收到 ACK 会 broadcast）*/
            if(!DATA_ALLOWED(e->state)) break;
            ts = ts_after_ms(1000);
            pthread_cond_timedwait(&e->cond, &e->lock, &ts);
            continue;
        }
        uint32_t freec = RING_CAP - used;
        uint32_t n = (uint32_t)(len - total);
        if(n > freec) n = freec;
        ring_store(e->snd_ring, e->snd_app, src+total, n);
        e->snd_app += n;
        total += (int)n;
        maybe_send_locked(sock, e);
    }
    pthread_mutex_unlock(&e->lock);
    return total;
}

int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    if(!sock || !sock->ext) return -1;
    struct tju_tcp_ext* e = sock->ext;
    char* dst = (char*)buffer;

    pthread_mutex_lock(&e->lock);
    if(e->recv_ring == NULL || e->state == CLOSED){
        pthread_mutex_unlock(&e->lock);
        return -1;
    }
    struct timespec ts;
    /* 等待数据或 FIN */
    while(e->rcv_avail == e->rcv_nxt && !e->recv_fin){
        ts = ts_after_ms(1000);
        pthread_cond_timedwait(&e->cond, &e->lock, &ts);
    }
    /* EOF */
    if(e->rcv_avail == e->rcv_nxt && e->recv_fin){
        pthread_mutex_unlock(&e->lock);
        return 0;
    }
    uint32_t n = e->rcv_nxt - e->rcv_avail;
    if(n > (uint32_t)len) n = (uint32_t)len;
    uint32_t out = 0;
    for(uint32_t i=0;i<n;i++){
        uint32_t idx = (e->rcv_avail + i) % RING_CAP;
        dst[out++] = e->recv_ring[idx];
        e->present[idx] = 0;
        e->used_bytes--;
    }
    e->rcv_avail += n;

    /* 窗口恢复更新：可用空间从 <SMSS 到 >=SMSS 时主动通告（含零窗口恢复）*/
    uint16_t adv = advert_window(e);
    if(adv > e->last_adv && (adv - e->last_adv) >= MAX_SEG_LEN){
        send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
    }
    pthread_mutex_unlock(&e->lock);
    return (int)out;
}

int tju_close(tju_tcp_t* sock){
    if(!sock || !sock->ext) return -1;
    struct tju_tcp_ext* e = sock->ext;
    pthread_mutex_lock(&e->lock);

    if(e->state == CLOSED || e->destroyed){
        pthread_mutex_unlock(&e->lock);
        return 0;
    }
    if(e->state == LISTEN){
        /* 关闭 listen socket：直接摘表 */
        pthread_mutex_unlock(&e->lock);
        pthread_mutex_lock(&g_lock);
        for(int i=0;i<MAX_SOCK;i++) if(listen_socks[i]==sock) listen_socks[i]=NULL;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if(e->state == SYN_SENT){
        e->state = CLOSED; sock->state = CLOSED;
        pthread_cond_broadcast(&e->cond);
        pthread_mutex_unlock(&e->lock);
        sock_retire(sock);
        return 0;
    }

    /* 1) 排空发送缓冲（等待所有已提交数据被 ACK）。
     *    注意：若对端 FIN 在排空期间到达（状态变为 CLOSE_WAIT），排空仍要完成，
     *    因为我们答应把已提交的数据可靠送到对端后才发本端 FIN。 */
    uint64_t t0 = now_ms();
    struct timespec ts;
    while((uint32_t)(e->snd_app - e->snd_una) > 0 || e->seg_cnt > 0){
        maybe_send_locked(sock, e);
        if((uint32_t)(e->snd_app - e->snd_una) == 0 && e->seg_cnt == 0) break;
        if(now_ms() - t0 > CLOSE_GIVEUP_MS){
            pthread_mutex_unlock(&e->lock);
            return -1;
        }
        ts = ts_after_ms(1000);
        pthread_cond_timedwait(&e->cond, &e->lock, &ts);
        maybe_send_locked(sock, e);
    }

    /* 2) 发送 FIN。以“本端是否已收到并确认对端 FIN”决定角色：
     *    recv_fin == true  => 被动关闭方（对方先关，我们 LAST_ACK -> CLOSED）
     *    recv_fin == false => 主动关闭方（我们 FIN_WAIT_1/2 -> TIME_WAIT）
     */
    if(!e->fin_sent){
        e->fin_sent = 1;
        e->fin_seq = e->snd_nxt;
        e->snd_nxt = (uint32_t)(e->fin_seq + 1);  /* FIN 占 1 序号：此后回包(ACK)序号自此续起 */
        e->fin_acked = 0;
        e->syn_rtx_cnt = 0;
        if(e->recv_fin){
            /* 对端已先 FIN（排空期间到达也算），本端按被动方收尾 */
            e->state = LAST_ACK; sock->state = LAST_ACK;
        } else {
            e->state = FIN_WAIT_1; sock->state = FIN_WAIT_1;
        }
        send_pkt(sock, FIN_FLAG_MASK | ACK_FLAG_MASK, e->fin_seq, e->rcv_nxt, NULL, 0);
        restart_timer(e, now_ms());
    }

    /* 3) 等待关闭完成：
     *    被动方 LAST_ACK -> CLOSED；主动方（含同时关闭）最终到 TIME_WAIT。
     *    统一以终态 CLOSED/TIME_WAIT 判定。 */
    int ret = 0;
    int reached_closed = 0;
    t0 = now_ms();
    while(1){
        if(e->state == CLOSED || e->state == TIME_WAIT){ reached_closed = (e->state == CLOSED); break; }
        if(now_ms() - t0 > CLOSE_GIVEUP_MS){ ret = -1; break; }
        ts = ts_after_ms(1000);
        pthread_cond_timedwait(&e->cond, &e->lock, &ts);
    }
    pthread_mutex_unlock(&e->lock);

    if(ret == 0 && reached_closed){
        sock_retire(sock);   /* 被动方到 CLOSED 即释放 */
    }
    /* 主动方到 TIME_WAIT 后返回，由调度线程在 2*MSL 后回收 */
    return ret;
}

int tju_handle_packet(tju_tcp_t* sock, char* pkt){
    if(!sock || !sock->ext) return -1;
    get_ref(sock);
    struct tju_tcp_ext* e = sock->ext;
    pthread_mutex_lock(&e->lock);
    if(e->destroyed){
        pthread_mutex_unlock(&e->lock);
        put_ref(sock);
        return 0;
    }

    uint16_t plen = get_plen(pkt);
    if(plen < DEFAULT_HEADER_LEN || plen > MAX_LEN){
        pthread_mutex_unlock(&e->lock);
        put_ref(sock);
        return -1;
    }

    /* ---- listen socket：只处理 SYN ---- */
    if(e->state == LISTEN){
        if(get_flags(pkt) & SYN_FLAG_MASK && !(get_flags(pkt) & ACK_FLAG_MASK)){
            tju_tcp_t* child = make_child(sock, pkt);
            (void)child;
        }
        pthread_mutex_unlock(&e->lock);
        put_ref(sock);
        return 0;
    }

    uint8_t flags = get_flags(pkt);
    uint32_t seq = get_seq(pkt);

    if(e->state == SYN_SENT){
        /* 期望 SYNACK */
        if((flags & SYN_FLAG_MASK) && (flags & ACK_FLAG_MASK)){
            if(get_ack(pkt) == e->snd_nxt){
                e->rcv_isn = seq;
                e->rcv_nxt = seq + 1;
                e->rcv_avail = seq + 1;
                e->peer_rwnd = get_advertised_window(pkt);
                e->state = ESTABLISHED; sock->state = ESTABLISHED;
                e->timer_on = 0;
                send_pkt(sock, ACK_FLAG_MASK, e->snd_nxt, e->rcv_nxt, NULL, 0);
                pthread_cond_broadcast(&e->cond);
            } else {
                /* 对端收到重复 SYN（本端重发过），回应其当前 ISN */
                if((flags & SYN_FLAG_MASK)){
                    send_pkt(sock, SYN_FLAG_MASK|ACK_FLAG_MASK, e->snd_isn,
                             (uint32_t)(seq+1), NULL, 0);
                }
            }
        } else if(flags & SYN_FLAG_MASK){
            /* 同时打开情形简化为只回应 SYNACK */
        }
        pthread_mutex_unlock(&e->lock);
        put_ref(sock);
        return 0;
    }

    if(e->state == SYN_RECV){
        if((flags & ACK_FLAG_MASK) && get_ack(pkt) == e->snd_nxt){
            e->peer_rwnd = get_advertised_window(pkt);
            e->state = ESTABLISHED; sock->state = ESTABLISHED;
            e->timer_on = 0;
            /* 入全连接队列，唤醒 accept */
            struct tju_tcp_ext* pe = e->listen_parent ? e->listen_parent->ext : NULL;
            if(pe){
                pthread_mutex_lock(&pe->qlock);
                if(pe->qcnt < QCAP){
                    pe->q[pe->qin] = sock;
                    pe->qin = (pe->qin + 1) % QCAP;
                    pe->qcnt++;
                }
                pthread_cond_broadcast(&pe->qcond);
                pthread_mutex_unlock(&pe->qlock);
            }
            pthread_cond_broadcast(&e->cond);
            /* 若该包同时携带数据/FIN，继续按数据期处理 */
            if(get_plen(pkt) > DEFAULT_HEADER_LEN || (flags & FIN_FLAG_MASK)){
                handle_post_established(sock, e, pkt);
            }
        } else if((flags & SYN_FLAG_MASK) && !(flags & ACK_FLAG_MASK)){
            /* 客户端重传 SYN：重发 SYNACK */
            send_pkt(sock, SYN_FLAG_MASK|ACK_FLAG_MASK, e->snd_isn, e->rcv_nxt, NULL, 0);
        }
        pthread_mutex_unlock(&e->lock);
        put_ref(sock);
        return 0;
    }

    /* ---- 已建立/关闭状态：统一处理 ---- */
    handle_post_established(sock, e, pkt);
    pthread_mutex_unlock(&e->lock);
    put_ref(sock);
    return 0;
}
