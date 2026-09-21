# -*- coding: utf-8 -*-
"""把实验原始数据(CSV)与拥塞窗口 trace 画成 PNG 图。
依赖: Pillow（PIL），不依赖 matplotlib。字体用 Windows 微软雅黑。
用法: python plot.py            # 读取同目录 data/*.csv，输出 figures/*.png
"""
import csv, os, re, io, math
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
FIGS = os.path.join(HERE, "figures")
os.makedirs(FIGS, exist_ok=True)

W, H = 960, 580
ML, MR, MT, MB = 95, 35, 55, 78
BLACK = (30, 30, 30)
GRID = (222, 226, 232)
AXIS = (90, 95, 105)


def font(size, bold=False):
    cands = [r"C:\Windows\Fonts\msyhbd.ttc" if bold else r"C:\Windows\Fonts\msyh.ttc",
             r"C:\Windows\Fonts\simhei.ttf", r"C:\Windows\Fonts\simsun.ttc"]
    for c in cands:
        if os.path.exists(c):
            try:
                return ImageFont.truetype(c, size)
            except Exception:
                pass
    return ImageFont.load_default()


def nice_step(rng, target=5):
    if rng <= 0:
        return 1.0
    raw = rng / target
    mag = 10 ** int(round(__import__("math").log10(raw)))
    for m in (1, 2, 2.5, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


class Chart:
    def __init__(self, title, xlabel, ylabel, xmin, xmax, ymin, ymax, logy=False):
        self.img = Image.new("RGB", (W, H), "white")
        self.d = ImageDraw.Draw(self.img)
        self.title, self.xlabel, self.ylabel = title, xlabel, ylabel
        self.xmin, self.xmax, self.ymin, self.ymax = xmin, xmax, ymin, ymax
        self.logy = logy
        self.legend = []

    def X(self, x):
        return ML + (x - self.xmin) / (self.xmax - self.xmin) * (W - ML - MR)

    def _ty(self, y):
        if self.logy:
            return math.log10(max(y, self.ymin))
        return y

    def Y(self, y):
        a, b, c = self._ty(y), self._ty(self.ymin), self._ty(self.ymax)
        return H - MB - (a - b) / (c - b) * (H - MT - MB)

    def frame(self, xticks, yticks):
        d = self.d
        for y in yticks:
            py = self.Y(y)
            d.line([ML, py, W - MR, py], fill=GRID)
            d.text((ML - 12, py), f"{y:g}", font=font(17), fill=AXIS, anchor="rm")
        for x in xticks:
            px = self.X(x)
            d.line([px, H - MB, px, MT], fill=GRID)
            d.text((px, H - MB + 10), f"{x:g}", font=font(17), fill=AXIS, anchor="ma")
        d.rectangle([ML, MT, W - MR, H - MB], outline=AXIS, width=2)
        d.text(((ML + W - MR) / 2, H - 32), self.xlabel, font=font(19), fill=BLACK, anchor="mm")
        tmp = Image.new("RGB", (360, 34), "white")
        td = ImageDraw.Draw(tmp)
        td.text((180, 17), self.ylabel, font=font(19), fill=BLACK, anchor="mm")
        self.img.paste(tmp.rotate(90, expand=True), (8, int((MT + H - MB) / 2) - 30))
        d = self.d = ImageDraw.Draw(self.img)
        d.text((ML, 22), self.title, font=font(22, True), fill=BLACK)

    def series(self, pts, color, label, marker="o"):
        d = self.d
        px = [(self.X(x), self.Y(y)) for x, y in pts]
        if len(px) > 1:
            d.line(px, fill=color, width=3, joint="curve")
        for (a, b) in px:
            r = 5
            if marker == "o":
                d.ellipse([a - r, b - r, a + r, b + r], fill="white", outline=color, width=3)
            else:
                d.rectangle([a - r, b - r, a + r, b + r], fill="white", outline=color, width=3)
        self.legend.append((color, label))

    def dashed_vlines(self, xs, color):
        d = self.d
        for x in xs:
            px = self.X(x)
            for y0 in range(MT, H - MB, 10):
                d.line([px, y0, px, min(y0 + 5, H - MB)], fill=color, width=2)

    def draw_legend(self, pos="tr"):
        d = self.d
        f = font(17)
        wmax = max(d.textlength(t, font=f) for _, t in self.legend) + 46
        hgt = 12 + 27 * len(self.legend)
        x1 = W - MR - 18
        x0 = x1 - wmax
        y0 = MT + 14
        d.rectangle([x0, y0, x1, y0 + hgt], fill="white", outline=(180, 185, 195))
        for i, (c, t) in enumerate(self.legend):
            y = y0 + 22 + i * 27
            d.line([x0 + 12, y, x0 + 38, y], fill=c, width=3)
            d.ellipse([x0 + 20, y - 5, x0 + 30, y + 5], fill="white", outline=c, width=3)
            d.text((x0 + 46, y), t, font=f, fill=BLACK, anchor="lm")

    def save(self, name):
        self.img.save(os.path.join(FIGS, name))
        print("saved:", os.path.join(FIGS, name))


def load_csv(name):
    path = os.path.join(DATA, name)
    if not os.path.exists(path):
        return []
    with io.open(path, encoding="utf-8") as f:
        return list(csv.DictReader(f))


def agg(rows, key, val="mbps"):
    """按 key 分组求均值/最小/最大"""
    g = {}
    for r in rows:
        g.setdefault(float(r[key]), []).append(float(r[val]))
    out = {}
    for k, v in g.items():
        out[k] = (sum(v) / len(v), min(v), max(v), len(v))
    return out


def log_ticks(lo, hi):
    """给出对数轴刻度（1/2/5 × 10^k 落在 [lo,hi] 内）"""
    out = []
    k = int(math.floor(math.log10(lo))) - 1
    while 10 ** k <= hi * 10:
        for m in (1, 2, 5):
            v = m * 10 ** k
            if lo <= v <= hi:
                out.append(v)
        k += 1
    return out


def sweep_figure(csv_a, csv_b, key, xlabel, title, oname, xmax=None, note=None):
    ra, rb = load_csv(csv_a), load_csv(csv_b)
    if not ra:
        print("skip (no data):", csv_a)
        return
    A = agg(ra, key)
    B = agg(rb, key) if rb else {}
    xs = sorted(set(A) | set(B))
    allv = [v[0] for v in list(A.values()) + list(B.values())]
    lo = min(allv) * 0.55 if allv else 0.1
    hi = max(allv) * 2.0 if allv else 10
    c = Chart(title, xlabel, "吞吐率 (MB/s，对数轴)", min(xs), (xmax or max(xs)), lo, hi, logy=True)
    c.frame(xs, log_ticks(lo, hi))
    c.series([(k, A[k][0]) for k in sorted(A)], (31, 119, 180), "配置A：不限制 cwnd（最终提交版）")
    if B:
        c.series([(k, B[k][0]) for k in sorted(B)], (214, 39, 40), "配置B：基础Reno 限窗 min(rwnd,cwnd)", "s")
    c.draw_legend()
    if note:
        c.d.text((ML + 10, H - MB - 16), note, font=font(15), fill=(200, 60, 60))
    c.save(oname)


def trace_figure(logname, oname, title):
    path = os.path.join(DATA, logname)
    if not os.path.exists(path):
        print("skip (no trace):", path)
        return
    t0, cwnd, ssth, loss = None, [], [], []
    pat = re.compile(r"\[CWND\]\s+([\d.]+)\s+(\w+)\s+cwnd=(\d+)\s+ssthresh=(\d+)\s+flight=(\d+)")
    for line in io.open(path, encoding="utf-8", errors="ignore"):
        m = pat.search(line)
        if not m:
            continue
        t, tag, cw, ss = float(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4))
        if t0 is None:
            t0 = t
        t -= t0
        cwnd.append((t, cw / 1024.0))
        ssth.append((t, ss / 1024.0))
        if tag in ("rto", "fast_retx"):
            loss.append((t, cw / 1024.0, tag))
    if not cwnd:
        print("no CWND samples in", path)
        return
    ymax = max(max(y for _, y in cwnd), max(y for _, y in ssth)) * 1.18
    tmax = max(t for t, _ in cwnd)
    xmax = tmax * 1.08 if tmax > 0 else 1.0
    step = nice_step(xmax, 5)
    c = Chart(title, "时间 (s)", "拥塞窗口 / ssthresh (KB)", 0, xmax, 0, ymax)
    c.frame([round(i * step, 3) for i in range(int(xmax / step) + 1)],
            [round(ymax * i / 4, 0) for i in range(5)])
    c.dashed_vlines([t for t, _, _ in loss], (255, 190, 190))
    c.series(ssth, (255, 165, 0), "ssthresh", "s")
    c.series(cwnd, (31, 119, 180), "cwnd")
    n_rto = sum(1 for _, _, tag in loss if tag == "rto")
    n_fr = sum(1 for _, _, tag in loss if tag == "fast_retx")
    c.d.text((ML + 8, MT + 8), f"红色虚线 = 丢包事件（RTO {n_rto} 次，快速重传 {n_fr} 次）",
             font=font(16), fill=(200, 60, 60))
    c.draw_legend(pos="tr")
    c.save(oname)


if __name__ == "__main__":
    sweep_figure("exp1_A.csv", "exp1_B.csv", "loss_pct", "丢包率 (%)",
                 "图1  吞吐率随丢包率变化（时延固定 6 ms，4 MB 传输，3 次均值）", "fig1_loss.png",
                 note="配置B（限窗）在丢包率 ≥1% 时 4 MB 传输在客户端 40 s 关闭上限内未完成")
    sweep_figure("exp2_A.csv", "exp2_B.csv", "delay_ms", "单向时延 (ms)",
                 "图2  吞吐率随链路时延变化（丢包固定 1%，4 MB 传输，2 次均值）", "fig2_delay.png",
                 note="该组链路含 1% 丢包，配置B（限窗）无法在 40 s 内完成，故仅有配置A曲线")
    trace_figure("cwnd_loss2.log", "fig3_cwnd_loss2.png",
                 "图3  拥塞窗口演化（Reno 限窗，丢包率 2%）")
    trace_figure("cwnd_loss0.log", "fig4_cwnd_loss0.png",
                 "图4  慢启动与拥塞避免（Reno 限窗，无丢包）")
