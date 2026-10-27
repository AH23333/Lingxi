#pragma once
//
// 托盘图标 + 设置窗口（LingxiUI 进程内）。
// - 托盘左键：开/关设置窗口；右键：菜单（设置 / 拼音模式开关 / 退出）
// - 设置即时保存到 HKCU\Software\LingxiIme，DLL/UI 下一次读取即生效
//
#include <windows.h>
#include <functional>

namespace lingxi {

// 创建隐藏主窗口 + 注册托盘图标（explorer 重启自动恢复）
void InitTrayAndSettings(HINSTANCE hInst);
// 打开设置窗口（托盘左键或命令行 --settings 调用）
void ShowSettingsWindow(HINSTANCE hInst);

// 注册"用户词典已变更（清空/导入）"回调，由 main 绑定到拼音引擎的 ReloadUserDict。
void SetUserDictReloadCallback(std::function<void()> cb);
// 注册"打开符号面板"回调，由 main 绑定（供托盘菜单调用）。
void SetShowSymbolsCallback(std::function<void()> cb);

} // namespace lingxi
