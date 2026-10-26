#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成灵犀输入法 M3 预测引擎的词表数据。

输入（同目录，需先下载）:
  ui/engine/data/g10k.txt        google-10000-english（按词频排序，每行一词）
  ui/engine/data/jieba_dict.txt  jieba 词库（词 词频 词性，MIT 许可）

输出:
  ui/engine/data/en_words.txt    word<TAB>rank（rank 小 = 常用）
  ui/engine/data/zh_words.txt    word<TAB>freq（按词频降序，含至少一个 CJK 字符）
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "ui", "engine", "data")

CJK_RE = re.compile(r"[\u4e00-\u9fff]")
ASCII_LETTER_RE = re.compile(r"[A-Za-z]")


def build_en():
    src = os.path.join(os.path.join(HERE, "..", "deps", "data-src", "g10k.txt"))
    dst = os.path.join(DATA, "en_words.txt")
    words = []
    seen = set()
    with open(src, encoding="utf-8") as f:
        for line in f:
            w = line.strip()
            if not w or w in seen:
                continue
            # 只保留纯英文字母词（撇号等留给后期），长度 1~20
            if not re.fullmatch(r"[a-zA-Z]{1,20}", w):
                continue
            seen.add(w)
            words.append(w)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        for rank, w in enumerate(words):
            f.write(f"{w}\t{rank}\n")
    print(f"en_words.txt: {len(words)} words")


def build_zh(cap=80000):
    src = os.path.join(os.path.join(HERE, "..", "deps", "data-src", "jieba_dict.txt"))
    dst = os.path.join(DATA, "zh_words.txt")
    entries = []
    with open(src, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ")
            if len(parts) < 2:
                continue
            w, freq = parts[0], parts[1]
            if not freq.isdigit():
                continue
            # 必须含 CJK 字符；排除含 ASCII 字母/数字的混排与纯符号
            if not CJK_RE.search(w) or ASCII_LETTER_RE.search(w) or re.search(r"[0-9]", w):
                continue
            try:
                entries.append((w, int(freq)))
            except ValueError:
                continue
    entries.sort(key=lambda e: -e[1])
    entries = entries[:cap]
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        for w, freq in entries:
            f.write(f"{w}\t{freq}\n")
    print(f"zh_words.txt: {len(entries)} entries (top {cap} by freq)")


if __name__ == "__main__":
    build_en()
    build_zh()
