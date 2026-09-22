#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py — 生成 UI 用的点阵字库 (core/font_atlas.h)

思路：
  1. 扫描 src/ 下所有 .c/.h 文件里出现的字符（UTF-8），加上 ASCII 可见字符与固定补充字符集；
  2. 用 PIL + SimHei.ttf 把每个字符按多个字号渲染成灰度（抗锯齿）位图；
  3. 文字字号用 4bit alpha 打包（体积减半，视觉几乎无差），特大字号用 8bit；
  4. 每号一张图集，zlib 压缩后写成 C 头文件，运行时由 stb 的 inflate 解压。

用法:
    python3 tools/gen_font.py            # 生成 core/font_atlas.h
    python3 tools/gen_font.py --check    # 只统计字号/字符数
"""
import os
import re
import sys
import zlib
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
FONT_PATH = os.path.join(HERE, "SimHei.ttf")
OUT_PATH = os.path.join(SRC, "core", "font_atlas.h")

# 扫描源码时先去掉注释，只统计真正会显示出来的文字
RE_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
RE_LINE_COMMENT = re.compile(r"//[^\n]*")

# (宏名, 像素大小, 位深)
SIZES = [
    ("FONT_SMALL", 15, 4),
    ("FONT_BODY", 19, 4),
    ("FONT_TITLE", 26, 4),
    ("FONT_HUGE", 44, 8),
]

# 特大字号只用于时钟/大数字，只收录这些字符
HUGE_CHARS = "0123456789:.-+ %"

# 始终包含的常用字（UI 文案会动态拼接，不依赖源码扫描）
EXTRA = (
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    " .,:;/\\|-—_+=*#%&@!?()[]{}<>\"'`~^$"
    "年月日时分秒"
    "星期一二三四五六"
    "张个段秒帧条项"
    "×·→←↑↓°"
    "拍照录像图库删除取消确定返回保存失败成功"
    "内置存储卡空间已用剩余大小"
    "正在加载请稍候无内容为空"
    "上一张下一张播放暂停继续"
    "今天昨天刚刚"
    "未检测到摄像头演示模式"
    "确定要删除此文件不可恢复"
    "AI助手你好我是有什么可以帮你的吗"
    "发送输入消息在线离线"
    "设置关于电量信号"
    "开始停止时长大小名称"
    "已保存已删除已取消"
    "正在生成视频"
    "秒前分钟小时"
    "重置相册提示警告错误"
    "对焦闪光灯切换"
    "全部照片视频媒体文件"
    "网络连接中已断开"
    "电池充电"
    "拍摄录制查看编辑"
    "文件夹路径"
    "设备连接异常请检查"
    "存储卡已满，请删除部分文件"
    "正在处理请勿断电"
    "张照片个视频"
    "第页共"
    "按下快门拍摄照片"
    "轻触开始录制"
    "按住说话"
    "键盘"
    "完成"
    "语言"
    "关于本机"
    "版本"
    "型号"
    "系统"
    "你好"
    "谢谢"
    "再见"
    "请问"
    "可以"
    "我们"
    "什么"
    "怎么"
    "为什么"
    "时间"
    "天气"
    "音乐"
    "视频"
    "图片"
    "开心"
    "哈哈"
    "微笑"
    "表情"
)


def collect_chars():
    """扫描源码收集所有用到的字符"""
    chars = set(EXTRA)
    for root, _dirs, files in os.walk(SRC):
        if "third_party" in root:
            continue
        for fn in files:
            if not fn.endswith((".c", ".h")):
                continue
            with open(os.path.join(root, fn), "rb") as f:
                data = f.read()
            text = data.decode("utf-8", "ignore")
            text = RE_BLOCK_COMMENT.sub(" ", text)
            text = RE_LINE_COMMENT.sub(" ", text)
            for ch in text:
                if ch in "\n\t\r":
                    continue
                chars.add(ch)
    return {c for c in chars if c.isprintable() and ord(c) >= 32}


def render_glyphs(charset, px):
    """渲染字符集 -> {ch: (w, h, bx, by, adv, bitmap)}"""
    font = ImageFont.truetype(FONT_PATH, px)
    ascent, descent = font.getmetrics()
    out = {}
    for ch in sorted(charset):
        try:
            bbox = font.getbbox(ch)
            adv = int(round(font.getlength(ch)))
        except Exception:
            continue
        if bbox is None:
            bbox = (0, 0, 0, 0)
        x0, y0, x1, y1 = bbox
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if w == 0 or h == 0:
            out[ch] = (0, 0, 0, 0, adv, b"")
            continue
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, fill=255, font=font)
        out[ch] = (w, h, x0, y0, adv, img.tobytes())
    return out, ascent, descent


def shelf_pack(entries, max_w):
    """按高度排序的货架装箱 -> (w, h, atlas_bytes, glyphs)"""
    items = [(ch, w, h, bx, by, adv, bm) for ch, (w, h, bx, by, adv, bm) in entries.items()
             if w > 0 and h > 0]
    x = y = row_h = 0
    placed = []
    for it in sorted(items, key=lambda t: -t[2]):
        w, h = it[1] + 1, it[2] + 1
        if x + w > max_w:
            x = 0
            y += row_h
            row_h = 0
        placed.append((it, x, y))
        x += w
        row_h = max(row_h, h)
    total_h = y + row_h
    atlas = bytearray(max_w * total_h)
    glyphs = []
    for (ch, w, h, bx, by, adv, bm), gx, gy in placed:
        for row in range(h):
            dst = (gy + row) * max_w + gx
            atlas[dst:dst + w] = bm[row * w:(row + 1) * w]
        glyphs.append((ord(ch), w, h, bx, by, adv, gx, gy))
    glyphs.sort(key=lambda g: g[0])     # 运行时二分查找
    return max_w, total_h, bytes(atlas), glyphs


def pack_bits(atlas, bpp):
    """8bit -> bpp 位打包（4bit 时两个像素一个字节，低位在前）"""
    if bpp == 8:
        return atlas
    assert bpp == 4
    out = bytearray((len(atlas) + 1) // 2)
    for i, v in enumerate(atlas):
        q = v >> 4          # 16 级足够文字抗锯齿
        if i & 1:
            out[i >> 1] |= q
        else:
            out[i >> 1] = q << 4
    return bytes(out)


def emit_bitmap(f, name, data):
    padded = data + b"\x00" * ((4 - len(data) % 4) % 4)
    f.write("static const uint32_t %s_bits[%d] = {\n" % (name, len(padded) // 4))
    for i in range(0, len(padded), 16):
        chunk = padded[i:i + 16]
        words = []
        for j in range(0, len(chunk), 4):
            w4 = chunk[j:j + 4]
            words.append("0x%02x%02x%02x%02x" % (w4[3], w4[2], w4[1], w4[0]))
        f.write("    " + ", ".join(words) + ",\n")
    f.write("};\n")


def main():
    check_only = "--check" in sys.argv
    all_chars = collect_chars()
    huge_chars = set(HUGE_CHARS)
    print("全量字符 %d 个（ASCII %d）; 特大字号 %d 个" %
          (len(all_chars), sum(1 for c in all_chars if ord(c) < 128), len(huge_chars)))

    sizes_out = []
    for name, px, bpp in SIZES:
        charset = huge_chars if name == "FONT_HUGE" else all_chars
        entries, ascent, descent = render_glyphs(charset, px)
        w, h, atlas, glyphs = shelf_pack(entries, 512)
        packed = pack_bits(atlas, bpp)
        comp = zlib.compress(packed, 9)
        print("  %-11s %2dpx %dbit 图集 %dx%d = %7d B -> zlib %6d B, 字形 %3d, ascent=%d"
              % (name, px, bpp, w, h, len(packed), len(comp), len(glyphs), ascent))
        sizes_out.append(dict(name=name, px=px, bpp=bpp, ascent=ascent, descent=descent,
                              w=w, h=h, comp=comp, glyphs=glyphs))

    if check_only:
        return

    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write("/* 本文件由 tools/gen_font.py 自动生成，请勿手工修改 */\n")
        f.write("#ifndef SHIXI_FONT_ATLAS_H\n#define SHIXI_FONT_ATLAS_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write("typedef struct {\n"
                "    uint32_t cp;      /* Unicode 码点 */\n"
                "    uint8_t  w, h;    /* 位图宽高 */\n"
                "    int8_t   bx, by;  /* 相对原点的偏移 */\n"
                "    uint8_t  adv;     /* 步进宽度 */\n"
                "    uint16_t gx, gy;  /* 图集内位置 */\n"
                "} FontGlyph;\n\n")
        f.write("typedef struct {\n"
                "    const char     *name;\n"
                "    int             px;\n"
                "    int             ascent, descent;\n"
                "    int             atlas_w, atlas_h;\n"
                "    int             bpp;        /* 4 或 8 */\n"
                "    const uint32_t *bits_z;     /* zlib 压缩的 alpha 图集 */\n"
                "    int             bits_z_len;\n"
                "    const FontGlyph *glyphs;\n"
                "    int             glyph_count;\n"
                "} FontAtlas;\n\n")

        for s in sizes_out:
            f.write("/* ---- %s: %dpx %dbit ---- */\n" % (s["name"], s["px"], s["bpp"]))
            emit_bitmap(f, s["name"], s["comp"])
            f.write("static const FontGlyph %s_glyphs[%d] = {\n" % (s["name"], len(s["glyphs"])))
            for cp, gw, gh, bx, by, adv, gx, gy in s["glyphs"]:
                f.write("    {0x%04x,%d,%d,%d,%d,%d,%d,%d},\n" %
                        (cp, gw, gh, bx, by, min(adv, 255), gx, gy))
            f.write("};\n\n")

        f.write("static const FontAtlas g_font_atlases[%d] = {\n" % len(sizes_out))
        for s in sizes_out:
            f.write('    {"%s", %d, %d, %d, %d, %d, %d, %s_bits, %d, %s_glyphs, %d},\n' %
                    (s["name"].lower(), s["px"], s["ascent"], s["descent"],
                     s["w"], s["h"], s["bpp"], s["name"], len(s["comp"]),
                     s["name"], len(s["glyphs"])))
        f.write("};\n\n#define FONT_ATLAS_COUNT %d\n\n" % len(sizes_out))
        f.write("#endif /* SHIXI_FONT_ATLAS_H */\n")

    print("已生成 %s (%.1f KB)" % (OUT_PATH, os.path.getsize(OUT_PATH) / 1024.0))


if __name__ == "__main__":
    main()
