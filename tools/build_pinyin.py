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
MODERN = os.path.join(HERE, "modern_terms.txt")
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


def build_from_jieba(cap=300000):
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


def inject_modern(base_lines, band=5000):
    """现代词层注入（词库类级修复，非逐词打补丁的通道）：

    jieba 基础源对现代领域词是**结构性缺口**，两类病：
      1) 整词不存在——提示词/大模型/自动补全…（打全拼只能逐字硬拼，用户报告
         "自动补全"只剩"自动+单字"即此类）；
      2) 存在但频次畸形——"补全"源频率=3 → rank 27.8 万，词链 cost 被
         "自动+不+全"的单字链反超。

    现代词层 deps/data-src/modern_terms.txt（人工注音 词<TAB>音节，不依赖
    pypinyin，可独立于全量重建维护）。语义=**只升不降的搬迁置顶**：词+读音
    已存在且原 rank 劣于 band → 摘除旧行插到 band（修复频次畸形）；原 rank
    已优于 band → 保留原位（绝不降级）；不存在 → 新增到 band。
    幂等：重跑位置稳定。返回 (新列表, 新增行数)。
    """
    modern = []
    if os.path.exists(MODERN):
        with open(MODERN, encoding="utf-8") as f:
            for ln in f:
                ln = ln.rstrip("\n")
                if not ln or "\t" not in ln:
                    continue
                w, py = ln.split("\t", 1)
                py = " ".join(py.split())
                if w and py:
                    modern.append((w, py))
    if not modern:
        return base_lines, 0

    def key_of(ln):
        if "\t" not in ln:
            return ("", "")
        w, py = ln.split("\t", 1)
        return (w, " ".join(py.split()))

    existing = {}
    for i, ln in enumerate(base_lines):
        existing.setdefault(key_of(ln), i)

    move_in, keep_already_good = [], []
    drop = set()
    for kp in modern:
        pos = existing.get(kp)
        if pos is None:
            move_in.append(kp)          # 新增
        elif pos >= band:
            move_in.append(kp)          # 频次畸形：摘旧插新
            drop.add(pos)
        else:
            keep_already_good.append(pos)  # 本来就好：不动

    out = [ln for i, ln in enumerate(base_lines) if i not in drop]
    band_now = min(band, len(out))
    ins = ["%s\t%s" % kp for kp in move_in]
    n_new = len(ins) - sum(1 for kp in move_in
                           if existing.get(kp) is not None)  # 纯新增计数
    _ = keep_already_good
    return out[:band_now] + ins + out[band_now:], n_new


def write_lines(lines):
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        for line in lines:
            f.write(line + "\n")


def apply_readings_fix():
    """多音字读音系统性补全（pypinyin 单词只出一套读音 → 口语常音缺口的类级修复）。
    见 fix_readings.py：数据交叉审计 + 幂等补全。full 重建与 overlay-only 两条
    路径统一在最后执行，防止管线任何一条再引入同类缺口。"""
    sys.path.insert(0, HERE)
    import fix_readings
    added = fix_readings.apply_to_file(OUT)
    print("readings-fix: +%d polyphonic single-char readings" % added)


def overlay_only():
    """读取现有 OUT，叠加覆盖层后重写（无需 pypinyin）。"""
    with open(OUT, encoding="utf-8") as f:
        base_lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    merged = apply_overlay(base_lines)
    merged, n_modern = inject_modern(merged)
    added = len(merged) - len(base_lines)
    write_lines(merged)
    apply_readings_fix()
    print("zh_pinyin.txt: %d entries (%+d from overlay, +%d modern)"
          % (len(merged), added, n_modern))


def modern_only():
    """仅重放现代词层+读音修复（对既有 OUT 幂等；日常补词走这条，免 pypinyin）。"""
    with open(OUT, encoding="utf-8") as f:
        base_lines = [ln.rstrip("\n") for ln in f if ln.strip()]
    merged, n_modern = inject_modern(base_lines)
    write_lines(merged)
    apply_readings_fix()
    print("zh_pinyin.txt: %d entries (+%d modern)" % (len(merged), n_modern))


def main():
    if "--overlay-only" in sys.argv:
        overlay_only()
        return
    if "--modern-only" in sys.argv:
        modern_only()
        return
    cap = 300000
    for i, a in enumerate(sys.argv):
        if a == "--cap" and i + 1 < len(sys.argv):
            cap = int(sys.argv[i + 1])
    base_lines = build_from_jieba(cap)
    merged = apply_overlay(base_lines)
    merged, _n = inject_modern(merged)
    write_lines(merged)
    apply_readings_fix()
    print("zh_pinyin.txt: %d entries (jieba cap=%d + spoken overlay + modern)"
          % (len(merged), cap))


if __name__ == "__main__":
    main()
