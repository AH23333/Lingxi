//
// uitheme.cpp — 主题模块实现（uitheme.h，R7 外观改造）。
// 背景：旧"跟随应用"依赖 MsgHello.darkTheme，而 ime/pipeclient.cpp 发送的
// darkTheme 恒为 0（宿主无法感知 DLL 宿主应用的主题），导致跟随模式永远
// 浅色。现改为宿主直读系统主题（SystemDarkMode），DLL 协议字段退役。
//
#include "uitheme.h"
#include "uistyle.h"

namespace lingxi {

// 系统当前是否深色：HKCU\...\Themes\Personalize!AppsUseLightTheme
// （DWORD：0=深色 1=浅色；键缺失=旧系统/未设置，按浅色）。
// 每次实时读取（µs 级，与 LoadSettings 同模式，免缓存失效管理）。
bool SystemDarkMode()
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    if (rc != ERROR_SUCCESS) return false;
    return light == 0;
}

// darkMode：0 跟随系统 1 浅色 2 深色（越界回退跟随系统）
bool EffectiveDark(uint32_t darkMode)
{
    switch (darkMode)
    {
    case 1: return false;
    case 2: return true;
    default: return SystemDarkMode();
    }
}

// 双主题调色板：浅色=uistyle.h 令牌原值（含 settingsui 旧字面色），深色=
// Win11 同级灰阶暗变体。设置页绘制（PaintSettings）与子控件着色
// （WM_CTLCOLOR*）共用；候选窗配色独立于本表（candwnd_render 自带暗色系）。
const UiPalette& UiPaletteFor(bool dark)
{
    static const UiPalette kLight = {
        kUiBg,               // bg
        kUiCard,             // card
        kUiCardBorder,       // cardBorder
        kUiText,             // text
        kUiText2,            // text2
        kUiControlEdge,      // controlEdge
        RGB(229, 229, 229),  // navPill（导航选中）
        RGB(238, 238, 238),  // navPillHover
        RGB(238, 238, 238),  // sep（行分隔线）
        RGB(234, 234, 234),  // toggleOff（药丸开关"关"底）
        RGB(90, 90, 90),     // toggleKnobOff
        RGB(255, 255, 255),  // btnText
    };
    static const UiPalette kDark = {
        RGB(32, 32, 32),     // bg（Win11 深色窗体底）
        RGB(43, 43, 43),     // card
        RGB(63, 63, 63),     // cardBorder
        RGB(240, 240, 240),  // text
        RGB(160, 160, 160),  // text2
        RGB(124, 124, 124),  // controlEdge
        RGB(58, 58, 58),     // navPill
        RGB(47, 47, 47),     // navPillHover
        RGB(51, 51, 51),     // sep
        RGB(66, 66, 66),     // toggleOff
        RGB(196, 196, 196),  // toggleKnobOff
        RGB(255, 255, 255),  // btnText
    };
    return dark ? kDark : kLight;
}

// 画刷小缓存：WM_CTLCOLOR 每次重绘都会问一遍，免重复建删。
// 刷子进程持有不回收（容量=全应用纯色数上限，个位数级）。
HBRUSH UiSolidBrush(COLORREF color)
{
    struct Entry { COLORREF c; HBRUSH b; };
    static Entry cache[24] = {};
    for (int i = 0; i < 24; i++)
    {
        if (cache[i].b && cache[i].c == color) return cache[i].b;
        if (!cache[i].b)
        {
            cache[i].c = color;
            cache[i].b = CreateSolidBrush(color);
            return cache[i].b;
        }
    }
    return (HBRUSH)GetStockObject(BLACK_BRUSH); // 超容量兜底（实际不可达）
}

// 标题栏深浅（Win10 1809+/Win11）：DWMWA_USE_IMMERSIVE_DARK_MODE(20)。
// 属性不支持静默忽略（老系统标题栏保持系统默认色）。
void UiApplyTitleBarDark(HWND hwnd, bool dark)
{
    HMODULE m = GetModuleHandleW(L"dwmapi.dll");
    if (!m) return;
    typedef HRESULT(WINAPI* PSet)(HWND, DWORD, const void*, DWORD);
    PSet p = (PSet)GetProcAddress(m, "DwmSetWindowAttribute");
    if (!p) return;
    BOOL on = dark ? TRUE : FALSE;
    p(hwnd, 20, &on, sizeof(on));
}

} // namespace lingxi
