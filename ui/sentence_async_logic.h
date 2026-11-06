#pragma once
//
// AI 整句异步追加（期二）纯逻辑层——pipeserver.cpp 的判定/塑形全走这里，
// engine_test 直接单测。无窗口/无线程/无引擎依赖（header-only）。
//
// 设计锚点（用户裁决，2026-11-05）：
//   触发限流 = 音节数≥6 + 候选流已出 + 键入停顿≥200ms，三者同时满足才生成；
//             停顿窗由 worker 线程 Sleep 后复查实现，本层只提供复查判定。
//   追加纪律 = 整句只追加在现有候选之后，带 anno 标记（显示 (AI)，不进上屏
//             文本），不参与前 5 位排序，不自动选中，不改 preedit。
//   陈旧丢弃 = ver（输入代数）/gen（选词·翻页代）/sentSeq（整句触发合并序号）
//             三重判定；单次生成软预算 1200ms，超时/迟到整条丢弃。
//
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/predictor.h"

namespace lingxi {

constexpr uint32_t kSentenceSettleMs = 200;     // 键入停顿观察窗（worker Sleep 后复查）
constexpr uint64_t kSentenceBudgetMs = 1200;    // 单次生成软预算（超时整条丢弃）
constexpr size_t kSentenceMinSyllables = 6;     // 触发音节数门槛（限流核心）
constexpr wchar_t kSentenceAnnoMark[] = L"AI";  // 候选注音位标记（渲染为 (AI)）

// 全拼串 → 音节切分：DP 取"音节数最小"的全覆盖切分——即用户的自然分词读法
//（"jintiantianqi"→jin-tian-tian-qi 共 4，而非 ji-n-ti-an 式碎切；触发计数
// 取自然音节数=保守口径，正是限流意图）。碎切候选因无法全覆盖自然淘汰
//（"ji" 合法但 "ntian" 非法）。
// 任一段非合法音节 / 含非 ASCII / 空串 → 返回空（双拼键码、v/u 模式串、
// 含数字等天然不触发）。返回值的 size() 即音节数。
inline std::vector<std::string> SegmentSyllables(const std::wstring& raw,
                                                 const std::unordered_set<std::string>& sylls)
{
    std::string s;
    s.reserve(raw.size());
    for (wchar_t ch : raw)
    {
        if (ch > 0x7F) return {};
        s.push_back((char)ch);
    }
    const size_t n = s.size();
    if (n == 0) return {};
    // dp[i] = s[0..i) 全覆盖的最小音节数（INT_MAX=不可覆盖）；par[i]=达成该值的上一刀位置
    std::vector<int> dp(n + 1, INT32_MAX), par(n + 1, -1);
    dp[0] = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (dp[i] == INT32_MAX) continue;
        for (size_t len = 1; len <= 6 && i + len <= n; len++) // 音节≤6 字符（pinyin 建表口径）
        {
            if (!sylls.count(s.substr(i, len))) continue;
            int cand = dp[i] + 1;
            if (cand < dp[i + len]) { dp[i + len] = cand; par[i + len] = (int)i; }
        }
    }
    if (dp[n] <= 0 || dp[n] == INT32_MAX) return {};
    std::vector<std::string> out;
    for (int cur = (int)n; cur > 0; cur = par[cur])
        out.push_back(s.substr((size_t)par[cur], (size_t)(cur - par[cur])));
    std::reverse(out.begin(), out.end());
    return out;
}

// 触发门（pipeserver 在首帧候选已发出后评估；任一条件破坏=不生成）：
//   pinyinFrame   本帧是拼音组合帧；
//   candsShown    候选流已出（首帧候选非空）；
//   syllables     SegmentSyllables 的音节数（门槛判定在本函数内）；
//   aiEnabled     设置开关（缺省开；关=整条通路不触发，候选与逐字一致）；
//   shuangpin     双拼模式（键码非拼音文本，不触发）；
//   calcOrRadical v 计算器 / u 拆字帧（确定性候选，不触发）。
// 停顿≥200ms 不在这里判——由 worker 线程停顿窗+陈旧复查完成（本层无从计时）。
inline bool ShouldTriggerSentence(bool pinyinFrame, bool candsShown, size_t syllables,
                                  bool aiEnabled, bool shuangpin, bool calcOrRadical)
{
    return pinyinFrame && candsShown && syllables >= kSentenceMinSyllables &&
           aiEnabled && !shuangpin && !calcOrRadical;
}

// 陈旧判定（worker 在停顿窗后、生成后、投递前各查一次；全部匹配才合法）：
//   alive/isActive       客户端仍连着且仍是活跃前台；
//   lastLearnVer==ver    没有更新的拼音预测覆盖本帧（用户继续键入 → 新 ver）；
//   genNow==gen          期间无选词/翻页（MSG_PICKED/MSG_PAGE 递增 gen）；
//   sentSeqNow==seq      没有更新的整句触发把本次合并掉。
inline bool SentenceStillCurrent(bool alive, bool isActive,
                                 uint32_t lastLearnVer, uint32_t ver,
                                 uint32_t genNow, uint32_t gen,
                                 uint32_t sentSeqNow, uint32_t seq)
{
    return alive && isActive && lastLearnVer == ver && genNow == gen && sentSeqNow == seq;
}

// 迟到丢弃：实测耗时超单次预算 → 整条丢（不留残句、不上屏，调用方负责留日志）。
inline bool ShouldDropLate(uint64_t elapsedMs, uint64_t budgetMs)
{
    return elapsedMs > budgetMs;
}

// 追加塑形：把整句接到现有候选末尾；满员时先丢最末一个常规候选腾位（第 60 位
// 之后本就翻不到）。返回 false=不追加（空句/与现有候选重复），调用方整条放弃。
// 纪律：绝不插入前部、绝不动前 5 位顺序；cover 平行时末位补 rawLen（整句消耗
// 全部原始输入，选中后组合串清空）；anno 平行时末位补 kSentenceAnnoMark（只影响
// 显示，不进上屏文本）；不平行则维持原状（不造半截平行数组）。
inline bool AppendSentence(Prediction& base, const std::wstring& sentence,
                           uint32_t rawLen, size_t maxCands)
{
    if (sentence.empty()) return false;
    for (const auto& w : base.cands)
        if (w == sentence) return false; // 与现有候选重复：追加=制造重号候选
    while (base.cands.size() >= maxCands && !base.cands.empty())
    {
        base.cands.pop_back();
        if (base.cover.size() == base.cands.size() + 1) base.cover.pop_back();
        if (base.anno.size() == base.cands.size() + 1) base.anno.pop_back();
    }
    bool hasCover = !base.cover.empty() && base.cover.size() == base.cands.size();
    bool hasAnno = base.anno.size() == base.cands.size() && !base.cands.empty();
    base.cands.push_back(sentence);
    if (hasCover) base.cover.push_back(rawLen);
    if (hasAnno) base.anno.push_back(kSentenceAnnoMark);
    return true;
}

} // namespace lingxi
