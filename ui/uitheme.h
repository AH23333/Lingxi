#pragma once
//
// 主题模块（R7 外观改造）：系统深浅色探测 + 生效主题解析 + 双主题调色板。
// - "跟随系统"：读 HKCU\...\Themes\Personalize!AppsUseLightTheme（每次查询
//   µs 级注册表读取，与 LoadSettings 同模式，无需缓存失效管理）；
//   系统切换广播 WM_SETTINGCHANGE("ImmersiveColorSet") 由设置窗自行响应。
// - EffectiveDark(darkMode)：0=跟随系统 1=浅色 2=深色，候选窗（pipeserver
//   每请求）与设置页（每次绘制）统一从这里取生效主题。
// - UiPaletteFor(dark)：Win11 灰阶双主题调色板（浅色=uistyle.h 令牌原值，
//   深色=同级对比度暗变体），设置页绘制与子控件着色（WM_CTLCOLOR*）共用。
// - UiSolidBrush：进程级小容量画刷缓存（WM_CTLCOLOR 高频路径免重复建刷）。
// - UiApplyTitleBarDark：DWMWA_USE_IMMERSIVE_DARK_MODE(20)——深色主题下
//   标题栏跟随变暗，浅色恢复；不支持（Win10 旧版）静默忽略。
//
#include <windows.h>
#include <cstdint>

namespace lingxi {

struct UiPalette
{
    COLORREF bg;            // 窗体底
    COLORREF card;          // 卡片/控件底
    COLORREF cardBorder;    // 卡片描边
    COLORREF text;          // 主文本
    COLORREF text2;         // 次级文本
    COLORREF controlEdge;   // 未勾选框描边
    COLORREF navPill;       // 导航选中药丸
    COLORREF navPillHover;  // 导航热态药丸
    COLORREF sep;           // 行分隔线
    COLORREF toggleOff;     // 药丸开关"关"底
    COLORREF toggleKnobOff; // 药丸开关"关"滑钮
    COLORREF btnText;       // 强调色按钮文字
};

bool SystemDarkMode();                 // 系统当前是否深色（读注册表）
bool EffectiveDark(uint32_t darkMode); // darkMode: 0 跟随系统 1 浅色 2 深色
const UiPalette& UiPaletteFor(bool dark);
HBRUSH UiSolidBrush(COLORREF color);   // 缓存画刷（勿 DeleteObject，进程回收）
void UiApplyTitleBarDark(HWND hwnd, bool dark);

} // namespace lingxi
