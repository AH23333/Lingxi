#pragma once
//
// v 模式：数字大写 + 计算器（全拼触发，组合串形如 "v123"/"v3*7"）。
// DLL 侧把 v 后的数字/运算符收进组合（composition.cpp CalcKeyChar），
// 引擎在拼音词典处理前拦截（pinyin.cpp Predict 入口）：
//   纯数字     → [大写, 小写, 人民币元角分]（小数 ≤2 位才有金额候选）
//   表达式     → [结果, 大写结果, 小写结果]（非法/除零/超界 → 空=仅回显）
// Learn 侧拒收：确定性结果不进自学习，防用户词典污染。
// 双拼模式下 v 为声母（zh），DLL 不触发，本模块自然不可达。
//
#include <string>
#include <vector>

namespace lingxi {

// 是否 v 模式查询："v"+数字/运算符（首个非 v 字符属计算字符集；"v"/"vlog" 非）。
bool IsCalcQuery(const std::wstring& s);

// v 模式候选（可能空=非法，仅回显）。cover 语义：整串消耗（调用方置 cover=串长）。
std::vector<std::wstring> CalcCandidates(const std::wstring& s);

} // namespace lingxi
