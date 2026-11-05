// caret_edit.h —— 组合内光标编辑纯逻辑（M5 尾巴落地，搜狗差距台账 #2）。
// caret 以"距串尾偏移"（tail）表示：缺省 0 = 光标在串尾（绝大多数输入全程
// 如此）；左移增大、右移减小。采用距尾偏移而非绝对位置的原因：拼音串任何
// 清零/重置路径（commit/cancel/重开组合共十余处）都无需挂钩——偏移在使用点
// 经 ClampTail 惰性收敛即自动合法。
#pragma once
#include <string>

namespace lingxi {

inline size_t ClampTail(size_t tail, size_t len) { return tail > len ? len : tail; }

// 左移：光标向串首靠一格（tail+1），已到串首（tail==len）不变。
inline size_t TailLeft(size_t tail, size_t len)
{
    tail = ClampTail(tail, len);
    return tail < len ? tail + 1 : tail;
}

// 右移：光标向串尾靠一格（tail-1），已在串尾（tail==0）不变。
inline size_t TailRight(size_t tail, size_t len)
{
    tail = ClampTail(tail, len);
    return tail > 0 ? tail - 1 : 0;
}

// 光标处插入：caret = len - tail；tail 不变（插入后光标右侧字符数不变）。
inline void TailInsert(std::wstring& s, size_t tail, wchar_t ch)
{
    tail = ClampTail(tail, s.size());
    s.insert(s.size() - tail, 1, ch);
}

// 光标处退格：删光标左侧一格；光标已在串首或串空 → false（调用方决定吞/取消）。
inline bool TailBackspace(std::wstring& s, size_t& tail)
{
    tail = ClampTail(tail, s.size());
    size_t pos = s.size() - tail;
    if (pos == 0) return false;
    s.erase(pos - 1, 1);
    return true;
}

// 光标绝对位置（0..len），供 TSF SetSelection 同步显示。
inline size_t TailCaretPos(size_t tail, size_t len) { return len - ClampTail(tail, len); }

} // namespace lingxi
