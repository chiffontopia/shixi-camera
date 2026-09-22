#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 core/pinyin_dict.h —— 拼音输入法用的「音节 -> 候选字」表。

为什么要生成而不是直接用现成的小表：
    原先那份手工表（tools/pinyin_dict_src.c，396 个音节 / 1366 个汉字）实测在
    日常提问里缺字率 **13.2%**（「摄像头」的摄像、「音乐播放器」的器、「饿」都打不出来），
    连它自己缺整个 e 块。这里改成用 pypinyin（MIT，数据源自 Unicode Unihan）铺满
    全部常用字，字形覆盖与字体（SimHei 28522 码点）对得上。

候选顺序（同一个音节里先出哪个字，直接影响「按空格取第一个」的手感）：
    1) 原手工表里已有的字，按原顺序（那份表是按常用度排的）
    2) GB2312 一级常用字（3755 个），按码点
    3) 其余字，按码点

用法：python3 tools/make_pinyin_dict.py       （需要 pip install pypinyin，仅主机端生成用）
"""
import re

from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SRC = HERE / "pinyin_dict_src.c"
OUT = ROOT / "core" / "pinyin_dict.h"

# 只收字体能画出来的范围（超出会显示成空心方框，不如不收）
RANGES = ((0x4E00, 0x9FFF), (0x3400, 0x4DBF))


def curated():
    """原手工表：音节 -> 候选串（同时作为排序依据）"""
    if not SRC.exists():
        return {}
    text = SRC.read_text(encoding="utf-8", errors="ignore")
    out = {}
    for py, hz in re.findall(r'\{\s*"([a-z]+)"\s*,\s*"([^"]*)"\s*\}', text):
        out.setdefault(py, hz)
    return out


def gb2312_level1():
    """GB2312 一级常用字（3755 个，0xB0A1..0xD7F9）"""
    s = set()
    for b1 in range(0xB0, 0xD8):
        for b2 in range(0xA1, 0xFF):
            try:
                c = bytes([b1, b2]).decode("gb2312")
            except Exception:
                continue
            if len(c) == 1:
                s.add(c)
    return s


def syllable_of(cp):
    """用 pypinyin 的公开接口取读音：它负责去声调、ü->v、大小写"""
    from pypinyin import pinyin, Style
    r = pinyin(chr(cp), style=Style.NORMAL, heteronym=False, errors=lambda x: None)
    if not r or not r[0] or not r[0][0]:
        return None
    s = r[0][0]
    return s if re.fullmatch(r"[a-z]+", s) else None


def main():
    from pypinyin.pinyin_dict import pinyin_dict

    cur = curated()
    l1 = gb2312_level1()

    syl = {}
    for cp in pinyin_dict:
        if not any(lo <= cp <= hi for lo, hi in RANGES):
            continue
        py = syllable_of(cp)
        if py:
            syl.setdefault(py, []).append(chr(cp))

    order_cur = {py: {c: i for i, c in enumerate(hz)} for py, hz in cur.items()}

    def rank(py, c):
        i = order_cur.get(py, {}).get(c)
        if i is not None:
            return (0, i)                      # 原表已有的字，按常用度顺序
        if c in l1:
            return (1, ord(c))                 # GB2312 一级常用字
        if ord(c) <= 0x9FFF:
            return (2, ord(c))                 # CJK 基本区其余字
        return (3, ord(c))                     # 扩展 A（生僻）垫底

    lines, total = [], 0
    for py in sorted(syl):
        chars = sorted(syl[py], key=lambda c: rank(py, c))
        # 原表里出现过的字若 pypinyin 没收录，也补上（避免丢字）
        for c in cur.get(py, ""):
            if c not in chars:
                chars.append(c)
        s = "".join(chars)
        total += len(s)
        lines.append('    { "%s", "%s" },' % (py, s))

    OUT.write_text(
        "/* 本文件由 tools/make_pinyin_dict.py 生成，请勿手改。\n"
        " * 数据来源：pypinyin（MIT，数据源自 Unicode Unihan）+ 原手工表的常用度排序。\n"
        " * 只收字体（SimHei）能画出来的 CJK 范围，见脚本里的 RANGES。\n"
        " */\n"
        "#ifndef SHIXI_PINYIN_DICT_H\n"
        "#define SHIXI_PINYIN_DICT_H\n\n"
        "typedef struct { const char *py; const char *hz; } PyEntry;\n\n"
        "static const PyEntry PY_DICT[] = {\n"
        + "\n".join(lines) + "\n"
        "};\n\n"
        "#define PY_DICT_COUNT ((int)(sizeof(PY_DICT) / sizeof(PY_DICT[0])))\n\n"
        "#endif /* SHIXI_PINYIN_DICT_H */\n",
        encoding="utf-8")

    print(f"音节 {len(lines)} 个，汉字 {total} 个，写入 {OUT.relative_to(ROOT)} "
          f"（{OUT.stat().st_size / 1024:.0f} KB）")
    for probe in ("e", "ei", "en", "er", "shi", "lv"):
        print(f"  {probe:<4} -> {syl.get(probe, [])[:8]}")


if __name__ == "__main__":
    main()
