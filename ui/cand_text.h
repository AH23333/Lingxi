// cand_text.h — 候选文本截断口径单一事实源（批9）
// 横排布局（candwnd_core LayoutAndPosition 列宽）与渲染（candwnd_render OnPaint）
// 必须同口径，否则列宽按未截断词测出、画布按截断词绘制，命中区与视觉错位。
#pragma once

#include <string>

namespace lingxi {

// 候选词截断：按码点计（UTF-16 代理对算 1 字，绝不劈开代理对——劈开的
// 高代理渲染为豆腐块且 GetTextExtentPoint 测宽失真），超过 maxChars 取前
// maxChars 码点 + 省略号。缺省 8 字 = 横排列宽 cap 口径。
inline std::wstring TruncateCandWord(const std::wstring& w, size_t maxChars = 8)
{
    size_t chars = 0, i = 0, cutUnits = 0;
    while (i < w.size())
    {
        size_t next = i + 1;
        if (w[i] >= 0xD800 && w[i] <= 0xDBFF && next < w.size() &&
            w[next] >= 0xDC00 && w[next] <= 0xDFFF)
            next = i + 2; // 代理对=一个字符
        ++chars;
        if (chars == maxChars) cutUnits = next;
        i = next;
    }
    if (chars <= maxChars) return w;
    return w.substr(0, cutUnits) + L"\u2026";
}

} // namespace lingxi
