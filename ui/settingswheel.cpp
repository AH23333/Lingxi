//
// 设置页滚轮转发（2026-11-01"组合框吃滚轮/误改设置"根治）。
// 悬停组合框/编辑框滚动时系统把滚轮发给该子控件：组合框直接吃掉（页面
// 不滚），且 CBS_DROPDOWNLIST 组合框按滚轮方向改选中项——用户设置被
// "滚"坏（暗色模式实感案例）。子类化把 WM_MOUSEWHEEL 原样转发父窗（设
// 置页自身的累积式滚动处理），子控件不再自行响应。下拉展开时 ComboLBox
// 是捕获态独立窗不经过此处，原生滚动下拉列表不受影响。
//
#include "uiinternal.h"

#include <windows.h>
#include <unordered_map>

namespace lingxi {

namespace {

// 已子类化子控件 → 原窗口 proc（WM_NCDESTROY 时还原，防悬垂 proc）
std::unordered_map<HWND, WNDPROC>& WheelMap()
{
    static std::unordered_map<HWND, WNDPROC> m;
    return m;
}

LRESULT CALLBACK SettingsWheelProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto& m = WheelMap();
    auto it = m.find(h);
    WNDPROC orig = (it != m.end()) ? it->second : nullptr;
    if (msg == WM_MOUSEWHEEL)
    {
        HWND parent = GetParent(h);
        if (parent) SendMessageW(parent, WM_MOUSEWHEEL, wParam, lParam);
        return 0; // 子控件不自行响应（组合框选中不被滚轮误改）
    }
    if (msg == WM_NCDESTROY && it != m.end())
    {
        m.erase(it);
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
    }
    return orig ? CallWindowProcW(orig, h, msg, wParam, lParam)
                : DefWindowProcW(h, msg, wParam, lParam);
}

BOOL CALLBACK WheelEnumProc(HWND h, LPARAM /*lParam*/)
{
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (lstrcmpiW(cls, L"COMBOBOX") == 0 || lstrcmpiW(cls, L"EDIT") == 0)
    {
        if (WheelMap().count(h)) return TRUE; // 已子类化（重复调用幂等）
        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SettingsWheelProc);
        if (orig) WheelMap().emplace(h, orig);
    }
    return TRUE;
}

} // namespace

void AttachSettingsWheelForwarding(HWND settingsWnd)
{
    if (!settingsWnd) return;
    EnumChildWindows(settingsWnd, WheelEnumProc, 0);
}

} // namespace lingxi
