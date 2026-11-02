#pragma once
//
// U 模式拆字（GAP §1，对齐微软拼音 U 模式）：'u'+部件读音 → 不会读的字。
// 全拼触发，引擎在拼音词典处理前拦截（pinyin.cpp Predict，st.shuangpin
// 门控——双拼下 'u' 是合法键位/声母，不接管）。表无前缀命中时落回正常
// 拼音 DP，B3 零声母简写兜底保活（"ui"→为、"uo"→我等无命中串不受影响）。
// 部件=一级直属部件、书写序（IDS 序，包围结构外框先）；不成字部首用俗称
// （氵shui 亻ren 忄xin 扌shou 钅jin 宀bao 辶zou 囗wei 灬huo 疒bing 等）。
// 排序：精确命中 > 已拼出部件多者 > 表序。cover 语义：整串消耗。
// Learn 侧拒收：IsRadicalQuery 键不进自学习（防词典污染）。
// 笔画输入（hspnzd）不在此模块（需逐字笔顺数据，另表，GAP 留待）。
//
#include <string>
#include <vector>

namespace lingxi {

// 是否 U 模式查询："u"+纯字母后缀且后缀为某条目键的前缀（小写化；
// 裸 "u"、非字母后缀、无表命中均非——落回正常拼音）。
bool IsRadicalQuery(const std::wstring& s);

// U 模式候选（可能空）。cover 语义：整串消耗（调用方置 cover=串长）。
std::vector<std::wstring> RadicalCandidates(const std::wstring& s);

} // namespace lingxi
