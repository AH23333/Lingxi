#pragma once
//
// WinUI/Fluent 风格设计令牌（浅色系）：中性灰阶窗体底 + 白色卡片/浮层 +
// 非蓝紫强调色皮肤。供候选窗/设置页/符号面板/快捷短语等全部 UI 共用；
// 皮肤（UiSkin）按 Settings.skinIndex 取色，驱动首行高亮、勾选框、按钮
// 等强调元素，全应用统一换肤。
//
#include <windows.h>
#include <cstdint>

namespace lingxi {

// ---- 皮肤：强调色 + 浅色主题 hover/选中底色（禁止蓝紫）----
struct SkinDef
{
    COLORREF accent;     // 强调色：候选首行/按钮/勾选框（配白字，需够深）
    COLORREF hoverLight; // 浅色主题 hover/选中底色（强调色的高亮度变体）
};

inline constexpr SkinDef kSkins[] = {
    { RGB(16, 124, 16),  RGB(230, 242, 230) }, // 0 翠绿（默认）
    { RGB(3, 131, 135),  RGB(224, 240, 241) }, // 1 青碧
    { RGB(202, 80, 16),  RGB(250, 234, 224) }, // 2 橙
    { RGB(209, 52, 56),  RGB(251, 232, 232) }, // 3 红
    { RGB(152, 111, 11), RGB(247, 240, 219) }, // 4 金
    { RGB(112, 76, 44),  RGB(238, 226, 214) }, // 5 茶棕
    { RGB(186, 26, 79),  RGB(250, 228, 236) }, // 6 玫粉
};
constexpr uint32_t kSkinCount = (uint32_t)(sizeof(kSkins) / sizeof(kSkins[0]));

// 取皮肤（越界回退 0）
inline const SkinDef& UiSkin(uint32_t idx)
{
    return kSkins[idx < kSkinCount ? idx : 0];
}

// ---- 界面字体族（Settings.uiFont 可选；候选窗/浮层共用）----
inline constexpr const wchar_t* kUiFontFaces[] = {
    L"Microsoft YaHei UI", // 0 微软雅黑（默认）
    L"SimSun",             // 1 宋体
    L"DengXian",           // 2 等线
    L"Segoe UI",           // 3 Segoe UI
};
inline constexpr const wchar_t* kUiFontNames[] = {
    L"微软雅黑", L"宋体", L"等线", L"Segoe UI",
};
constexpr uint32_t kUiFontCount =
    (uint32_t)(sizeof(kUiFontFaces) / sizeof(kUiFontFaces[0]));

// ---- WinUI 中性灰阶（浅色主题）----
constexpr COLORREF kUiBg           = RGB(243, 243, 243); // 窗体底
constexpr COLORREF kUiCard         = RGB(255, 255, 255); // 卡片/控件底
constexpr COLORREF kUiCardBorder   = RGB(229, 229, 229); // 卡片描边
constexpr COLORREF kUiFlyoutBg     = RGB(253, 253, 253); // 浮层（候选窗/符号面板）底
constexpr COLORREF kUiFlyoutBorder = RGB(224, 224, 224); // 浮层描边
constexpr COLORREF kUiText         = RGB(26, 26, 26);    // 主文本
constexpr COLORREF kUiText2        = RGB(93, 93, 93);    // 次级文本
constexpr COLORREF kUiControlEdge  = RGB(130, 130, 130); // 未勾选框描边

// ---- 浮层圆角 ----
// Win11：DWMWA_WINDOW_CORNER_PREFERENCE=DWMWCP_ROUND（系统级抗锯齿圆角，
// 一次登记后续尺寸变化自动跟随）。老系统退回 CreateRoundRectRgn（随尺寸
// 变化需重设——调用方须在每次改窗口尺寸后重新调用）。
inline void UiApplyRoundCorners(HWND hwnd, int w, int h)
{
    const int kSlots = 8;
    static HWND s_dwm[kSlots] = {};
    static bool s_dwmBad = false;
    if (!s_dwmBad)
    {
        bool known = false;
        for (int i = 0; i < kSlots; i++)
        {
            if (s_dwm[i] == hwnd) { known = true; break; }
            if (!s_dwm[i] || !IsWindow(s_dwm[i])) { s_dwm[i] = hwnd; break; }
        }
        if (known) return; // 已应用 DWM 圆角
        HMODULE m = GetModuleHandleW(L"dwmapi.dll");
        if (m)
        {
            typedef HRESULT(WINAPI* PSet)(HWND, DWORD, const void*, DWORD);
            PSet p = (PSet)GetProcAddress(m, "DwmSetWindowAttribute");
            DWORD pref = 2; // DWMWCP_ROUND
            if (p && SUCCEEDED(p(hwnd, 33, &pref, sizeof(pref)))) return;
        }
        s_dwmBad = true; // 本机不支持 DWM 圆角：后续一律 RGN
    }
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 8, 8);
    SetWindowRgn(hwnd, rgn, TRUE);
}

// Win11 给顶层窗默认画 1px 深色边框线（候选窗上=黑描边观感）。
// DWMWA_BORDER_COLOR(34) = DWMWA_COLOR_NONE 关掉它；属性不支持则静默忽略。
inline void UiDisableFrameBorder(HWND hwnd)
{
    HMODULE m = GetModuleHandleW(L"dwmapi.dll");
    if (!m) return;
    typedef HRESULT(WINAPI* PSet)(HWND, DWORD, const void*, DWORD);
    PSet p = (PSet)GetProcAddress(m, "DwmSetWindowAttribute");
    if (!p) return;
    COLORREF none = (COLORREF)0xFFFFFFFE; // DWMWA_COLOR_NONE
    p(hwnd, 34, &none, sizeof(none));
}

} // namespace lingxi
