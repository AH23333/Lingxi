#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fix_readings.py —— 单字多读音系统性补全（词典数据自身交叉审计，不依赖 pypinyin）。

根因：build_pinyin 用 pypinyin 默认模式逐词取拼音，每个词只出一套读音；
"谁" 被固定成书面音 shui，口语常音 shei 只活在"鹿死谁手/谁的心"这类词里，
单字条目永远取不到 → 用户打 shei 出不来"谁"。这是一类多音字的整体缺陷
（血/得/露/逮/薄……同病），必须类级修，不允许逐字手工打补丁。

做法（全数据驱动、幂等、可审计）：
  1) 遍历所有多字词行，仅在"音节数 == 字数"严格对齐时取 (字→音节) 证据，
     证据强度用该词行号（词频序）做代理；
  2) 对每个单字条目：证据读音集 − 已登记读音集 = 缺口；
     仅补证据词落在表前 1/3（足够常用）的读音，防生僻专名噪声；
  3) 应用时把 `字\t读音` 行紧跟该单字原行插入（rank 相邻，同音桶内按词频合理）。

用法:
  python tools/fix_readings.py --dry    # 只写审计报告 tmp/readings_audit.txt，不改数据
  python tools/fix_readings.py --check  # 审计后仍有缺口则退出码 1（回归闸）
  python tools/fix_readings.py          # 应用到 ui/engine/data/zh_pinyin.txt
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DICT = os.path.normpath(os.path.join(HERE, '..', 'ui', 'engine', 'data', 'zh_pinyin.txt'))
AUDIT = os.path.normpath(os.path.join(HERE, '..', 'tmp', 'readings_audit.txt'))
CJK1 = re.compile(r'^[\u4e00-\u9fff]$')
CJKALL = re.compile(r'^[\u4e00-\u9fff]+$')


def read_lines(path=None):
    with open(path or DICT, encoding='utf-8', newline='') as f:
        return [l.rstrip('\r\n') for l in f]


def analyse(lines):
    """返回 (additions, singles, evidence_min, first_idx)；additions: {字: [(读音, 证据行)]}"""
    evidence = {}   # 字 -> {读音: 最小行号}
    singles = {}    # 单字 -> 已登记读音集
    first_idx = {}  # 字 -> 首次出现的行号（排序/报告）
    for idx, line in enumerate(lines):
        if '\t' not in line:
            continue
        w, py = line.split('\t', 1)
        if not CJKALL.match(w):
            continue
        sylla = py.split()
        if not sylla:
            continue
        if len(w) == 1:
            if line.startswith(w + '\t'):
                first_idx.setdefault(w, idx)
            singles.setdefault(w, set()).add(sylla[0])
            continue
        if len(sylla) != len(w):
            continue  # 儿化/错位行不吃
        first_idx.setdefault(w[0], first_idx.get(w[0], idx))
        for ch, syl in zip(w, sylla):
            ev = evidence.setdefault(ch, {})
            if syl not in ev or idx < ev[syl]:
                ev[syl] = idx
    threshold = len(lines) // 3
    additions = {}
    for ch, cur in singles.items():
        gap = [(s, i) for s, i in sorted(evidence.get(ch, {}).items(), key=lambda kv: kv[1])
               if s not in cur and i <= threshold]
        if gap:
            additions[ch] = gap
    return additions, singles, first_idx


def write_report(additions, singles, first_idx, lines):
    total = sum(len(v) for v in additions.values())
    rep = ['缺口单字数: %d  缺口读音条目总数: %d  (词典共 %d 行)'
           % (len(additions), total, len(lines)),
           '格式: 字 [已登记读音] +缺读音(证据词行号)']
    for ch in sorted(additions, key=lambda c: first_idx.get(c, len(lines))):
        got = '/'.join(sorted(singles[ch]))
        want = ' '.join('%s(%d)' % (s, i) for s, i in additions[ch])
        rep.append('%s [%s] +%s' % (ch, got, want))
    d = os.path.dirname(AUDIT)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(AUDIT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(rep) + '\n')
    print(rep[0])
    print('audit ->', AUDIT)
    return total


def apply_to_file(path=None):
    """对词典文件执行幂等多读音补全，返回新增行数。供 CLI 与 build_pinyin.py 共用。"""
    path = path or DICT
    lines = read_lines(path)
    additions, singles, _ = analyse(lines)
    if not additions:
        return 0
    out = []
    done = set()
    for line in lines:
        out.append(line)
        if '\t' not in line:
            continue
        w = line.split('\t', 1)[0]
        if len(w) != 1 or w not in additions or (w, line) in done:
            continue
        done.add((w, line))  # 同一单字行只插一次（补全行再遇时跳过）
        for syl, _ in additions[w]:
            if syl not in singles[w]:
                out.append('%s\t%s' % (w, syl))
                singles[w].add(syl)  # 防同字他行重复插同读音
    tmp = path + '.tmpfix'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    os.replace(tmp, path)
    return len(out) - len(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry', action='store_true', help='仅审计报告，不改数据')
    ap.add_argument('--check', action='store_true', help='有缺口退出码 1，不改数据')
    args = ap.parse_args()

    lines = read_lines()
    additions, singles, first_idx = analyse(lines)
    total = write_report(additions, singles, first_idx, lines)
    if args.check:
        sys.exit(1 if total else 0)
    if args.dry:
        sys.exit(0)
    added = apply_to_file()
    print('applied: +%d lines -> %s' % (added, DICT))


if __name__ == '__main__':
    main()
