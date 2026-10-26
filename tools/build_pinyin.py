#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 M5 拼音输入的数据表：词 → 拼音序列。

输入: deps/data-src/jieba_dict.txt（词 词频 词性）
输出: ui/engine/data/zh_pinyin.txt   word<TAB>pinyin1 pinyin2 ...（按词频降序）

多音字处理：pypinyin lazy_pinyin 默认取最常见读音，够 M5 基础版用；
整词级多音分歧（如"重庆"）可后续加自定义词典。
"""
import os
import re
import sys

from pypinyin import lazy_pinyin, Style

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "deps", "data-src", "jieba_dict.txt")
OUT = os.path.join(HERE, "..", "ui", "engine", "data", "zh_pinyin.txt")

CJK_RE = re.compile(r"[\u4e00-\u9fff]")
ASCII_RE = re.compile(r"[A-Za-z0-9]")

def main(cap=80000):
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

    ok = 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for w, freq in entries:
            try:
                py = lazy_pinyin(w, style=Style.NORMAL, errors=lambda c: [None])
            except Exception:
                continue
            if any(s is None for s in py):
                continue  # 含无法转拼音的字符，跳过
            seq = " ".join(s.lower() for s in py)
            f.write(f"{w}\t{seq}\n")
            ok += 1
    print(f"zh_pinyin.txt: {ok} entries (top {cap})")

if __name__ == "__main__":
    main()
