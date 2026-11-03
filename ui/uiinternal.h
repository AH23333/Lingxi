#pragma once
//
// settingsui.cpp 拆分（P1 重构，2026-10-31）后的文件间内部共享面。
// 拆分：settingsui.cpp（设置窗）/ tray.cpp（托盘+主窗）/ userdictwnd.cpp
// （用户词典窗+存储）/ helpwnd.cpp（帮助窗）/ statswnd.cpp（统计窗）/
// settingsbackup.cpp（设置备份）。本头只声明跨文件符号；各文件私有
// 状态一律留在各自匿名命名空间。对外公共 API 仍走 settingsui.h 等
// 公共头；本头仅限上述拆分文件与 main.cpp（g_hooks 装配）使用，不得扩散。
//
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include "engine/predictor.h" // UserDictEntry（词典管理钩子参数）

namespace lingxi {

// ---- 全局回调收口（P3 重构，2026-10-31）----
// 旧 6 组"文件级全局 + SetXxxCallback setter"（g_reloadUserDict/g_showSymbols/
// g_settingsChanged/g_reqPinToggle/g_phChanged/g_hpChanged）归并为本结构：
// 消费点统一读 g_hooks.xxx；唯一写点=main.cpp 装配段一次性填充。
struct UiHooks
{
    std::function<void()> reloadUserDict;    // 用户词典变更后引擎重载
    std::function<void()> showSymbols;       // 托盘菜单：打开符号面板
    std::function<void()> settingsChanged;   // 设置保存后（同步热键等）
    std::function<void()> pinToggle;         // 托盘菜单：切换候选窗位置固定
    std::function<void()> phrasesChanged;    // 自定义短语保存后（与用户词典同一热载通道）
    std::function<void()> hotPhrasesChanged; // 快捷短语保存后重注册全局热键
    // 用户词典引擎原子操作（2026-11-01 删除复活根治）：引擎持锁改内存+落盘
    // 并等收敛；UI 不再直写词典文件（旧模型与引擎后台落盘竞态，在途旧快照
    // 会把已删词条写回"复活"）。key 空=该词全部读音键。
    std::function<bool(const std::wstring&, const std::wstring&)> removeUserWord; // (word,key) 删除
    std::function<bool()> clearUserDict; // 清空全部词条
    std::function<bool(const std::wstring&, const std::wstring&, int)> addUserWord; // (word,key,gain) upsert
    std::function<bool(const std::wstring&, const std::wstring&, int)> nudgeUserWord; // (word,key,±delta) 调频
    std::function<bool(const std::vector<UserDictEntry>&)> mergeUserDict; // 导入合并
    std::function<void()> flushUserDict; // L7：导出/备份前在途学习落盘收敛
    // A 细胞词库（cellwnd.cpp）：装/删包后请求引擎重扫（只置原子标志）；
    // 导入线程取音节表快照做词条校验（_syllables 构造后只读，拷贝安全）。
    std::function<void()> reloadCellPacks;
    std::function<bool(std::vector<std::string>&)> getSyllables;
};
extern UiHooks g_hooks; // 定义于 settingsui.cpp

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
void AttachSettingsWheelForwarding(HWND settingsWnd); // settingswheel.cpp：组合框/编辑框滚轮转发设置窗

// ---- userdictwnd.cpp：用户词典窗 + 词典存储 ----
void CreateUserDictWindow(HINSTANCE hInst);
// ---- cellwnd.cpp：A 细胞词库管理窗（design-cell-dict-2026-11-03 A5）----
void ShowCellDictWindow(HINSTANCE hInst);
std::wstring UserDictPathW();    // %APPDATA%\LingxiIme\userdict.txt
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileBytes(const std::wstring& path, const std::string& data);
bool UserDictReplaceFromRaw(const std::string& rawUtf8); // 备份恢复：清空+导入走引擎原子操作

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

// ---- main.cpp ----
// --diagnostics 全文（打字统计窗"导出诊断报告"复用）
std::wstring BuildDiagnosticsText();

} // namespace lingxi
