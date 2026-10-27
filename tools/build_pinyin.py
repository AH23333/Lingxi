#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成拼音输入的数据表：词 → 拼音序列。

输入: deps/data-src/jieba_dict.txt（词 词频 词性）
输出: ui/engine/data/zh_pinyin.txt   word<TAB>pinyin1 pinyin2 ...（按词频降序）

用法:
  python build_pinyin.py               # 完整重建（jieba + pypinyin，需安装 pypinyin）
  python build_pinyin.py --overlay-only  # 仅在现有词表上叠加口语覆盖层（无需 pypinyin）

口语覆盖层 tools/spoken_overlay.txt：
  jieba 词典偏书面、缺高频口语应答（"好的/好吗/你好吗"等），且把"你好/谢谢"
  这类高频词排得很靠后。覆盖层中的词（手工拼音、按真实常用度排列）会被置顶，
  并从基础表中去重。已存在于基础表的词沿用其原拼音（避免手工误差），仅调整位置。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "deps", "data-src", "jieba_dict.txt")
OUT = os.path.join(HERE, "..", "ui", "engine", "data", "zh_pinyin.txt")
OVERLAY = os.path.join(HERE, "spoken_overlay.txt")

CJK_RE = re.compile(r"[一-鿿]")
ASCII_RE = re.compile(r"[A-Za-z0-9]")


def load_overlay():
    """返回 [(word, pinyin_line), ...]，按文件顺序（即目标常用度）。"""
    items = []
    if not os.path.exists(OVERLAY):
        return items
    with open(OVERLAY, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or "\t" not in line:
                continue
            w, py = line.split("\t", 1)
            items.append((w.strip(), py.strip()))
    return items


def apply_overlay(base_lines):
    """把覆盖层词置顶并去重；已存在词沿用基础表原拼音。"""
    overlay = load_overlay()
    src_py = {}
    for line in base_lines:
        if "\t" in line:
            w, py = line.split("\t", 1)
            src_py.setdefault(w, py)

    merged = []
    seen = set()
    for w, py in overlay:
        if w in seen:
            continue
        merged.append("%s\t%s" % (w, src_py.get(w, py)))
        seen.add(w)
    for line in base_lines:
        line = line.rstrip("\n")
        if not line:
            continue
        w = line.split("\t", 1)[0]
        if w in seen:
            continue
        merged.append(line)
        seen.add(w)
    return merged


def build_from_jieba(cap=80000):
    """完整重建：jieba 词频排序 + pypinyin 转拼音，再叠加口语覆盖层。"""
    from pypinyin import lazy_pinyin, Style

    entries = []
    with open(SRC, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ")
            if len(parts) < 2 or not parts[1].isdigit():
                continue
            w, freq = parts[0], int(parts[1])
            if not CJK_RE.search(w) or ASCII_RE.search(w):
                continue
            entries.append((w, freq))
    entries.sort(key=lambda e: -e[1])
    entries = entries[:cap]

    base_lines = []
    for w, freq in entries:
        try:
            py = lazy_pinyin(w, style=Style.NORMAL, errors=lambda c: [None])
        except Exception:
            continue
        if any(s is None for s in py):
            continue  # 含无法转拼音的字符，跳过
        base_lines.append("%s\t%s" % (w, " ".join(s.lower() for s in py)))
    return base_lines


def write_lines(lines):
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")


def overlay_only():
    """读取现有 OUT，叠加覆盖层后重写（无需 pypinyin）。"""
    with open(OUT, encoding="utf-8") as f:
        base_lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    merged = apply_overlay(base_lines)
    added = len(merged) - len(base_lines)
    write_lines(merged)
    print("zh_pinyin.txt: %d entries (%+d from overlay)" % (len(merged), added))


def main():
    if "--overlay-only" in sys.argv:
        overlay_only()
        return
    base_lines = build_from_jieba()
    merged = apply_overlay(base_lines)
    write_lines(merged)
    print("zh_pinyin.txt: %d entries (jieba + spoken overlay)" % len(merged))


if __name__ == "__main__":
    main()
