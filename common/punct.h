#pragma once
//
// 中文标点映射（纯函数，DLL 与 engine_test 共用）。
// 输入虚拟键码 + Shift 状态，输出 1~2 个全角标点；成对引号由 dqOpen/sqOpen
// 记录开合状态并在命中时翻转。返回字符数；0 表示该键不映射（保持半角/另作他用，
// 如无 Shift 的 -/= 在拼音活动时作翻页、无 Shift 数字用于选词）。
//
#include <cstdint>
#include <string>
#include <windows.h>

namespace lingxi {

inline int MapChinesePunct(WPARAM vk, bool shift, bool& dqOpen, bool& sqOpen,
                           wchar_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    switch (vk)
    {
    case VK_OEM_PERIOD: out[0] = shift ? L'\u300B' /*》*/ : L'\u3002' /*。*/; return 1;
    case VK_OEM_COMMA:  out[0] = shift ? L'\u300A' /*《*/ : L'\uFF0C' /*，*/; return 1;
    case VK_OEM_2:      out[0] = shift ? L'\uFF1F' /*？*/ : L'\u3001' /*、*/; return 1; // / → 顿号
    case VK_OEM_5:      out[0] = shift ? L'\uFF5C' /*｜*/ : L'\u3001' /*、*/; return 1; // \ → 顿号
    case VK_OEM_1:      out[0] = shift ? L'\uFF1A' /*：*/ : L'\uFF1B' /*；*/; return 1;
    case VK_OEM_3:      out[0] = shift ? L'\uFF5E' /*～*/ : L'\u00B7' /*·*/; return 1;
    case VK_OEM_4:      out[0] = shift ? L'\uFF5B' /*｛*/ : L'\u3010' /*【*/; return 1;
    case VK_OEM_6:      out[0] = shift ? L'\uFF5D' /*｝*/ : L'\u3011' /*】*/; return 1;
    case VK_OEM_7:
        if (shift) { out[0] = dqOpen ? L'\u201D' /*”*/ : L'\u201C' /*“*/; dqOpen = !dqOpen; }
        else       { out[0] = sqOpen ? L'\u2019' /*’*/ : L'\u2018' /*‘*/; sqOpen = !sqOpen; }
        return 1;
    case VK_OEM_MINUS:
        if (shift) { out[0] = L'\u2014'; out[1] = L'\u2014'; return 2; } // 破折号 ——
        return 0; // 无 Shift 保留半角 -（拼音活动时作翻页）
    case VK_OEM_PLUS:
        if (shift) { out[0] = L'\uFF0B' /*＋*/; return 1; }
        return 0; // 无 Shift 保留半角 =（拼音活动时作翻页）
    case L'1': if (shift) { out[0] = L'\uFF01' /*！*/; return 1; } return 0;
    case L'2': if (shift) { out[0] = L'\u00B7' /*·*/; return 1; } return 0; // @ → 间隔号
    case L'3': if (shift) { out[0] = L'\uFF03' /*＃*/; return 1; } return 0;
    case L'4': if (shift) { out[0] = L'\uFFE5' /*￥*/; return 1; } return 0;
    case L'5': if (shift) { out[0] = L'\uFF05' /*％*/; return 1; } return 0;
    case L'6': if (shift) { out[0] = L'\u2026'; out[1] = L'\u2026'; return 2; } return 0; // ……
    case L'7': if (shift) { out[0] = L'\uFF06' /*＆*/; return 1; } return 0;
    case L'8': if (shift) { out[0] = L'\u00D7' /*×*/; return 1; } return 0;
    case L'9': if (shift) { out[0] = L'\uFF08' /*（*/; return 1; } return 0;
    case L'0': if (shift) { out[0] = L'\uFF09' /*）*/; return 1; } return 0;
    }
    return 0;
}

// 全角字母数字（搜狗 F4 对标，纯函数，DLL 直出与原文上屏共用消费）：
// ASCII 可见字符 0x21..0x7E 映射到全角 U+FF01..FF5E，空格映射全角空格 U+3000，
// 范围外原样返回。
inline wchar_t ToFullWidthW(wchar_t c)
{
    if (c == L' ') return L'\u3000';
    if (c >= L'!' && c <= L'~') return (wchar_t)(c - L'!' + L'\uFF01');
    return c;
}

inline std::wstring ToFullWidthStr(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) r += ToFullWidthW(c);
    return r;
}

} // namespace lingxi
