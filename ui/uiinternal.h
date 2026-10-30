#pragma once
//
// settingsui.cpp 拆分（P1 重构，2026-10-31）后的文件间内部共享面。
// 拆分：settingsui.cpp（设置窗）/ tray.cpp（托盘+主窗）/ userdictwnd.cpp
// （用户词典窗+存储）/ helpwnd.cpp（帮助窗）/ statswnd.cpp（统计窗）/
// settingsbackup.cpp（设置备份）。本头只声明跨文件符号；各文件私有
// 状态一律留在各自匿名命名空间。对外公共 API 仍走 settingsui.h 等
// 公共头，本头不得被 main.cpp/window.cpp 之外的范围扩散使用。
//
#include <windows.h>
#include <string>
#include <functional>

namespace lingxi {

// ---- 跨文件回调（定义于 settingsui.cpp；公开 setter 见 settingsui.h）----
extern std::function<void()> g_reloadUserDict;   // 用户词典变更后引擎重载
extern std::function<void()> g_showSymbols;      // 托盘菜单：打开符号面板
extern std::function<void()> g_settingsChanged;  // 设置保存后（同步热键等）
extern std::function<void()> g_reqPinToggle;     // 托盘菜单：切换候选窗固定

// ---- 共享资源（g_hIcon 定义于 tray.cpp；画刷/字体定义于 settingsui.cpp）----
extern HICON g_hIcon;
extern HBRUSH g_hBgBrush;  // WinUI 中性窗体底
extern HBRUSH g_hCardBrush; // WinUI 卡片白
extern HFONT g_hFont;      // 12px 通用控件字体（用户词典窗沿用）

// ---- settingsui.cpp：设置窗 ----
void CreateSettingsWindow(HINSTANCE hInst);
void SaveSettingsWindowRect();   // 设置窗矩形落盘（托盘隐藏路径复用）
void RefreshSettingsControls();  // 注册表被托盘等外部改写后同步设置窗控件
bool IsSettingsWindowVisible();  // 托盘左键开/关切换判定
void HideSettingsWindow();       // 托盘左键关闭路径（矩形落盘+隐藏）

// ---- userdictwnd.cpp：用户词典窗 + 词典存储 ----
void CreateUserDictWindow(HINSTANCE hInst);
std::wstring UserDictPathW();    // %APPDATA%\LingxiIme\userdict.txt
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileBytes(const std::wstring& path, const std::string& data);

// ---- helpwnd.cpp：使用帮助窗 ----
void ShowHelpWindow(HINSTANCE hInst);

// ---- statswnd.cpp：打字统计窗 ----
void ShowUsageStatsWindow(HINSTANCE hInst);

// ---- tray.cpp：托盘 ----
void UpdateTrayTip();            // 暂停等状态变化后刷新托盘悬停文字
bool IsAutoStartEnabled();       // HKCU\Run\LingxiIme 存在即开

// ---- settingsbackup.cpp：设置备份 ----
void ExportSettingsBackup(HWND owner);
void ImportSettingsBackup(HWND owner);

} // namespace lingxi
