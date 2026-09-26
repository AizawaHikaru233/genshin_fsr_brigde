#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FSR2 输入纹理转储分析（纯标准库，不需要 numpy / PIL）

配套 Dx11FsrBridge 的 `Fsr2InputDump` 诊断开关。用来回答：

    为什么画面上某一小块区域拿不到时间抗锯齿？

超分参数分两层：全局参数（渲染分辨率 / 抖动相位 / 锐化）整帧只有一份，
**不可能局部不同**；只有逐像素输入纹理（运动矢量 / 深度 / reactive / 透明）能局部不同。
所以本脚本把每张输入按 16×16 网格分块统计，并**按"偏离整帧常态的程度"排序**，
把可疑区域直接指出来。

用法：
    python fsr2-dump-analyze.py <fsr2dump 目录> [--tiles 16] [--crops 4]

输出：
    - 控制台报告（每帧：各纹理的异常分块排行 + 判读建议）
    - report.txt（同样内容落盘）
    - crop_f<帧>_<纹理>_t<行>_<列>.bmp（把最可疑的分块放大导出，可直接双击查看）

判读速查（报告里也会写）：
    motion 呈「整片中性(≈0.498039)」而周围不是  ⇒ 该区域几乎拿不到重投影 ⇒ 历史被拒 ⇒ 无时间抗锯齿
    reactive(B 通道) 在该区域饱和            ⇒ 被标记为"变化区域"，历史被钳制
    depth 在该区域异常(0 / 饱和 / 与周围断层) ⇒ 被判为 disocclusion，历史被拒
    以上三者都正常、而颜色在该区域高频变化    ⇒ 更可能是 FSR2 的亮度/细节稳定性逻辑在拒历史
"""

import os
import struct
import sys

RAW_MAGIC = b"FDMP"
RAW_HEADER_SIZE = 64

# DXGI_FORMAT 子集（与 Fsr2InputDump.cpp 的 bytes_per_pixel / decode_channels 对齐）
BPP = {
    2: 16,   # R32G32B32A32_FLOAT
    10: 8,   # R16G16B16A16_FLOAT
    16: 8,   # R32G32_FLOAT
    19: 8,   # R32G8X24_TYPELESS
    20: 8,   # D32_FLOAT_S8X24_UINT
    21: 8,   # R32_FLOAT_X8X24_TYPELESS
    23: 4,   # R10G10B10A2_TYPELESS  ← 本游戏的 motion
    24: 4,   # R10G10B10A2_UNORM
    26: 4,   # R11G11B10_FLOAT
    27: 4, 28: 4, 29: 4,   # R8G8B8A8 TYPELESS/UNORM/UNORM_SRGB
    34: 4,   # R16G16_FLOAT
    39: 4,   # R32_TYPELESS
    40: 4,   # D32_FLOAT
    41: 4,   # R32_FLOAT
    45: 4,   # D24_UNORM_S8_UINT
    46: 4,   # R24_UNORM_X8_TYPELESS
    53: 2, 54: 2, 55: 2, 56: 2,   # R16_TYPELESS / R16_FLOAT / D16_UNORM / R16_UNORM
    60: 1, 61: 1,   # R8_TYPELESS / R8_UNORM
    87: 4, 90: 4,   # B8G8R8A8_UNORM / _TYPELESS
}
FMT_NAME = {
    2: "R32G32B32A32_FLOAT", 10: "R16G16B16A16_FLOAT", 16: "R32G32_FLOAT",
    19: "R32G8X24_TYPELESS", 20: "D32_FLOAT_S8X24_UINT", 21: "R32_FLOAT_X8X24_TYPELESS",
    23: "R10G10B10A2_TYPELESS", 24: "R10G10B10A2_UNORM", 26: "R11G11B10_FLOAT",
    27: "R8G8B8A8_TYPELESS", 28: "R8G8B8A8_UNORM", 29: "R8G8B8A8_UNORM_SRGB",
    34: "R16G16_FLOAT", 39: "R32_TYPELESS", 40: "D32_FLOAT", 41: "R32_FLOAT",
    45: "D24_UNORM_S8_UINT", 46: "R24_UNORM_X8_TYPELESS",
    53: "R16_TYPELESS", 54: "R16_FLOAT", 55: "D16_UNORM", 56: "R16_UNORM",
    60: "R8_TYPELESS", 61: "R8_UNORM", 87: "B8G8R8A8_UNORM", 90: "B8G8R8A8_TYPELESS",
}
DEPTH_FMTS = {19, 20, 21, 39, 40, 45, 46, 55, 56}
MOTION_FMTS = {23, 24}

MOTION_NEUTRAL = 0.498039


def _half_to_float(bits):
    sign = (bits >> 15) & 1
    exp = (bits >> 10) & 0x1F
    man = bits & 0x3FF
    if exp == 0:
        v = (man / 1024.0) * (2.0 ** -14)
    elif exp == 31:
        v = 0.0 if man else 1e30
    else:
        v = (1.0 + man / 1024.0) * (2.0 ** (exp - 15))
    return -v if sign else v


def read_raw(path):
    """返回 (w, h, fmt, row_pitch, frame_index, data)"""
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < RAW_HEADER_SIZE or blob[:4] != RAW_MAGIC:
        raise ValueError("不是 FDMP 转储文件: %s" % path)
    # 布局（与 Fsr2InputDump.cpp 的 make_raw_header 对齐，已被 C++ 单测固定）：
    #   magic(4) version(u32) width(u32) height(u32) dxgi_format(u32) row_pitch(u32) frame(u64)
    _ver, w, h, fmt, pitch, frame = struct.unpack_from("<IIIIIQ", blob, 4)
    return w, h, fmt, pitch, frame, blob[RAW_HEADER_SIZE:]


def decode_px(fmt, data, off):
    """解出 (r, g, b, a)；未知格式返回 None。"""
    if fmt in (28, 29, 27):          # R8G8B8A8
        return data[off] / 255.0, data[off + 1] / 255.0, data[off + 2] / 255.0, data[off + 3] / 255.0
    if fmt in (87, 90):              # B8G8R8A8
        return data[off + 2] / 255.0, data[off + 1] / 255.0, data[off] / 255.0, data[off + 3] / 255.0
    if fmt in MOTION_FMTS:           # 10:10:10:2
        v = struct.unpack_from("<I", data, off)[0]
        return ((v & 0x3FF) / 1023.0, ((v >> 10) & 0x3FF) / 1023.0,
                ((v >> 20) & 0x3FF) / 1023.0, ((v >> 30) & 3) / 3.0)
    if fmt in (60, 61):              # R8_TYPELESS / R8_UNORM
        g = data[off] / 255.0
        return g, g, g, 1.0
    if fmt in (54,):                 # R16_FLOAT
        g = _half_to_float(struct.unpack_from("<H", data, off)[0])
        return g, g, g, 1.0
    if fmt == 34:                    # R16G16_FLOAT
        h0, h1 = struct.unpack_from("<HH", data, off)
        return _half_to_float(h0), _half_to_float(h1), 0.0, 1.0
    if fmt == 10:                    # R16G16B16A16_FLOAT
        h0, h1, h2, h3 = struct.unpack_from("<HHHH", data, off)
        return (_half_to_float(h0), _half_to_float(h1), _half_to_float(h2), _half_to_float(h3))
    if fmt in (39, 40, 41, 16, 2):   # R32/D32 系 + R32G32/R32G32B32A32（后两者取 r）
        g = struct.unpack_from("<f", data, off)[0]
        return g, g, g, 1.0
    if fmt in (20, 19):              # 深度在前 4 字节
        g = struct.unpack_from("<f", data, off)[0]
        return g, g, g, 1.0
    if fmt in (55, 56):              # D16_UNORM / R16_UNORM
        g = struct.unpack_from("<H", data, off)[0] / 65535.0
        return g, g, g, 1.0
    if fmt in (45, 46):              # R24_UNORM_X8
        v = struct.unpack_from("<I", data, off)[0] & 0xFFFFFF
        g = v / 16777215.0
        return g, g, g, 1.0
    return None


def decode_motion(r, g):
    """与 Ffx12Backend.cpp 的 CS 一致：d = raw - 0.498039; mv = -sign(d) * 4 * d^2"""
    def sq(d):
        m = -4.0 * d * d
        return m if d >= 0.0 else -m
    return sq(r - MOTION_NEUTRAL), sq(g - MOTION_NEUTRAL)


class Grid(object):
    """按 tiles×tiles 分块累积统计。"""

    def __init__(self, w, h, tiles):
        self.w, self.h, self.tiles = w, h, tiles
        self.n = [0] * (tiles * tiles)
        self.sum = [0.0] * (tiles * tiles)
        self.sumsq = [0.0] * (tiles * tiles)
        self.neutral = [0] * (tiles * tiles)
        self.sat = [0] * (tiles * tiles)

    def tile_of(self, x, y):
        tx = min(self.tiles - 1, x * self.tiles // max(1, self.w))
        ty = min(self.tiles - 1, y * self.tiles // max(1, self.h))
        return ty * self.tiles + tx

    def add(self, x, y, value, neutral=False, saturated=False):
        t = self.tile_of(x, y)
        self.n[t] += 1
        self.sum[t] += value
        self.sumsq[t] += value * value
        if neutral:
            self.neutral[t] += 1
        if saturated:
            self.sat[t] += 1

    def mean(self, t):
        return self.sum[t] / self.n[t] if self.n[t] else 0.0

    def std(self, t):
        if self.n[t] < 2:
            return 0.0
        m = self.mean(t)
        var = max(0.0, self.sumsq[t] / self.n[t] - m * m)
        return var ** 0.5

    def frac_neutral(self, t):
        return self.neutral[t] / self.n[t] if self.n[t] else 0.0

    def frac_sat(self, t):
        return self.sat[t] / self.n[t] if self.n[t] else 0.0

    def global_mean(self):
        tot = sum(self.n)
        return (sum(self.sum) / tot) if tot else 0.0

    def global_std(self):
        tot = sum(self.n)
        if tot < 2:
            return 0.0
        m = self.global_mean()
        var = max(0.0, sum(self.sumsq) / tot - m * m)
        return var ** 0.5


def write_bmp(path, w, h, rows_bgr):
    """rows_bgr: 每行一个 bytes（BGR，自顶向下）。写 24 位 BMP。"""
    stride = (w * 3 + 3) & ~3
    pad = b"\x00" * (stride - w * 3)
    body = b"".join(rows_bgr[y] + pad for y in range(h - 1, -1, -1))  # BMP 自底向上
    info_size = 40
    file_size = 14 + info_size + len(body)
    hdr = b"BM" + struct.pack("<IHHI", file_size, 0, 0, 14 + info_size)
    info = struct.pack("<IiiHHIIiiII", info_size, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + info + body)


def crop_bmp(out_path, data, w, h, pitch, fmt, tx, ty, tiles, up):
    """把 [tx,ty] 分块【最近邻放大 up 倍】导出为 BMP —— 放大才能看清边缘锯齿。"""
    x0 = tx * w // tiles
    x1 = max(x0 + 1, (tx + 1) * w // tiles)
    y0 = ty * h // tiles
    y1 = max(y0 + 1, (ty + 1) * h // tiles)
    x1 = min(x1, w)
    y1 = min(y1, h)
    sw, sh = x1 - x0, y1 - y0
    if sw < 1 or sh < 1:
        return False
    bpp = BPP.get(fmt, 4)
    rows = []
    for sy in range(sh):
        row = bytearray()
        base = (y0 + sy) * pitch
        for sx in range(sw):
            px = decode_px(fmt, data, base + (x0 + sx) * bpp)
            if px is None:
                bgr = b"\x00\x00\x00"
            elif fmt in MOTION_FMTS:
                mx, my = decode_motion(px[0], px[1])
                r = int(max(0.0, min(1.0, 0.5 + mx * 4.0)) * 255)
                g = int(max(0.0, min(1.0, 0.5 + my * 4.0)) * 255)
                bgr = bytes((0, g, r))
            elif fmt in DEPTH_FMTS:
                g = int(max(0.0, min(1.0, px[0])) * 255)
                bgr = bytes((g, g, g))
            else:
                clamp = lambda v: int(max(0.0, min(1.0, v if v <= 1.0 else v / (1.0 + v))) * 255)
                bgr = bytes((clamp(px[2]), clamp(px[1]), clamp(px[0])))
            row += bgr * up
        for _ in range(up):
            rows.append(bytes(row))
    write_bmp(out_path, sw * up, sh * up, rows)
    return True


def analyze_texture(name, path, tiles, sample_step, out_dir, frame, crops, crop_up):
    w, h, fmt, pitch, _frame, data = read_raw(path)
    bpp = BPP.get(fmt)
    gm = Grid(w, h, tiles)
    gmv = Grid(w, h, tiles)
    gre = Grid(w, h, tiles)
    motion_like = fmt in MOTION_FMTS
    depth_like = fmt in DEPTH_FMTS
    unknown = bpp is None or len(data) < 8 or decode_px(fmt, data, 0) is None

    if unknown:
        return {"name": name, "fmt": fmt, "unknown": True, "w": w, "h": h}

    for y in range(0, h, sample_step):
        base = y * pitch
        for x in range(0, w, sample_step):
            px = decode_px(fmt, data, base + x * bpp)
            if px is None:
                continue
            if motion_like:
                mx, my = decode_motion(px[0], px[1])
                mag = (mx * mx + my * my) ** 0.5
                is_neutral = abs(px[0] - MOTION_NEUTRAL) < 2.0 / 1023.0 and \
                    abs(px[1] - MOTION_NEUTRAL) < 2.0 / 1023.0
                gm.add(x, y, mag, neutral=is_neutral)
                gre.add(x, y, px[2], saturated=(px[2] > 0.75))
            elif depth_like:
                gm.add(x, y, px[0])
            else:
                lum = 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]
                gm.add(x, y, lum)

    result = {"name": name, "w": w, "h": h, "fmt": fmt, "tiles": tiles,
              "motion_like": motion_like, "depth_like": depth_like,
              "gmean": gm.global_mean(), "gstd": gm.global_std(),
              "grid": gm, "reactive": gre if motion_like else None}

    # 排行：偏离整帧常态的程度（用稳健的 z 分数；σ=0 时按绝对偏离排）
    sigma = result["gstd"] or 1e-6
    ranked = []
    for t in range(tiles * tiles):
        if gm.n[t] == 0:
            continue
        z = abs(gm.mean(t) - result["gmean"]) / sigma
        extra = 0.0
        if motion_like:
            # 该块"中性比例"与整帧中性比例的偏离，是"拿不到重投影"的直接指纹
            allneutral = sum(gm.neutral) / max(1, sum(gm.n))
            extra = abs(gm.frac_neutral(t) - allneutral) * 3.0
            if gre.n[t]:
                allsat = sum(gre.sat) / max(1, sum(gre.n))
                extra += abs(gre.frac_sat(t) - allsat) * 2.0
        ranked.append((z + extra, t, gm.mean(t), gm.frac_neutral(t),
                       gm.frac_sat(t) if motion_like and gre.n[t] else 0.0))
    ranked.sort(reverse=True)
    result["ranked"] = ranked

    # 导出最可疑分块的放大图
    crops_made = []
    for i, (_score, t, _m, _fn, _fs) in enumerate(ranked[:crops]):
        ty, tx = divmod(t, tiles)
        out = os.path.join(out_dir, "crop_f%d_%s_t%d_%d.bmp" % (frame, name, ty, tx))
        if crop_bmp(out, data, w, h, pitch, fmt, tx, ty, tiles, crop_up):
            crops_made.append(os.path.basename(out))
    result["crops"] = crops_made
    return result


def frame_report(frame, metas, results, tiles, lines):
    def emit(s=""):
        print(s)
        lines.append(s)

    emit("=" * 78)
    emit("帧 %d" % frame)
    emit("=" * 78)
    for k in ("render", "display", "jitter", "motion_scale", "reset",
              "use_reactive_mask", "use_transparency_mask", "sharpening"):
        if k in metas:
            emit("  %-22s %s" % (k, metas[k]))
    emit("")
    emit("  %-14s %-22s %8s %8s %10s" % ("纹理", "格式", "均值", "标准差", "采样"))
    for r in results:
        if r.get("unknown"):
            emit("  %-14s %-22s   （格式未知，跳过统计）" % (r["name"], FMT_NAME.get(r["fmt"], str(r["fmt"]))))
            continue
        emit("  %-14s %-22s %8.4f %8.4f %10d" % (
            r["name"], FMT_NAME.get(r["fmt"], str(r["fmt"])), r["gmean"], r["gstd"],
            sum(r["grid"].n)))
    emit("")

    for r in results:
        if r.get("unknown") or not r["ranked"]:
            continue
        emit("  --- %s：最偏离整帧常态的 %d 个分块（行,列 = 网格坐标）" % (r["name"], min(6, len(r["ranked"]))))
        for score, t, m, fn, fs in r["ranked"][:6]:
            ty, tx = divmod(t, tiles)
            extra = ""
            if r["motion_like"]:
                extra = "  中性比例=%.2f  饱和比例=%.2f" % (fn, fs)
            emit("      块(%2d,%2d)  偏离=%.2f  均值=%.4f%s" % (ty, tx, score, m, extra))
        if r["crops"]:
            emit("      放大图: %s" % ", ".join(r["crops"]))
        emit("")

    # 综合判读
    emit("  --- 判读建议")
    motion = next((r for r in results if r.get("motion_like") and not r.get("unknown")), None)
    if motion and motion["ranked"]:
        top = motion["ranked"][0]
        avg_neutral = sum(motion["grid"].neutral) / max(1, sum(motion["grid"].n))
        emit("     整帧 motion 中性像素比例 = %.3f" % avg_neutral)
        if top[3] > avg_neutral + 0.25:
            ty, tx = divmod(top[1], tiles)
            emit("     ⚠️ 块(%d,%d) 的 motion 中性比例(%.2f)显著高于整帧(%.2f)："
                 % (ty, tx, top[3], avg_neutral))
            emit("        该区域几乎拿不到重投影 ⇒ FSR2 会每帧拒绝历史 ⇒")
            emit("        表现就是「完全没有任何抗锯齿」。这与你看到的翅膀区域症状一致。")
        else:
            emit("     motion 没有出现明显的「中性孤岛」；若翅膀仍无 AA，")
            emit("     请重点看 depth 与 reactive 两张图的排行，以及颜色块的高频程度。")
    if motion and motion["reactive"]:
        red = motion["reactive"]
        sat = [t for t in range(tiles * tiles) if red.n[t] and red.frac_sat(t) > 0.5]
        if sat:
            emit("     ⚠️ reactive(motion 的 B 通道) 在 %d 个分块上饱和 >50%%：" % len(sat))
            emit("        这些区域会被判定为「变化区域」，历史被钳制。")
            emit("        可试 Fsr2UseReactiveMask=0（默认已是 0）或核对该通道语义。")
        else:
            emit("     reactive(B 通道) 未出现饱和分块。")
    emit("")


def load_meta(path):
    meta = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("input ") and not line.startswith("files"):
                    k, v = line.split("=", 1)
                    meta[k.strip()] = v.strip()
    except OSError:
        pass
    return meta


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    dump_dir = argv[1]
    tiles, crops, crop_up = 16, 4, 2
    sample_budget = 200000
    for i, a in enumerate(argv):
        if a == "--tiles" and i + 1 < len(argv):
            tiles = max(2, int(argv[i + 1]))
        if a == "--up" and i + 1 < len(argv):
            crop_up = max(1, int(argv[i + 1]))
        if a == "--crops" and i + 1 < len(argv):
            crops = max(0, int(argv[i + 1]))
    if not os.path.isdir(dump_dir):
        print("目录不存在: %s" % dump_dir)
        return 2

    frames = {}
    for fn in os.listdir(dump_dir):
        if fn.startswith("f") and fn.endswith("_meta.txt"):
            try:
                frames[int(fn[1:].split("_")[0])] = fn
            except ValueError:
                pass
    if not frames:
        print("在 %s 里没找到 f*_meta.txt —— 先开启 Fsr2InputDump=1 跑一次游戏。" % dump_dir)
        return 2

    lines = []
    for frame in sorted(frames):
        meta = load_meta(os.path.join(dump_dir, frames[frame]))
        results = []
        for name in ("motion", "depth", "color", "transparency", "output"):
            p = os.path.join(dump_dir, "f%d_%s.raw" % (frame, name))
            if not os.path.isfile(p):
                continue
            try:
                _w, _h, _f, _pitch, _fr, _d = read_raw(p)
            except (ValueError, OSError) as e:
                print("  跳过 %s: %s" % (os.path.basename(p), e))
                continue
            step = max(1, int((_w * _h / float(sample_budget)) ** 0.5))
            results.append(analyze_texture(name, p, tiles, step, dump_dir, frame, crops, crop_up))
        frame_report(frame, meta, results, tiles, lines)

    with open(os.path.join(dump_dir, "report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("报告已写入: %s" % os.path.join(dump_dir, "report.txt"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
