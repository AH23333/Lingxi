#pragma once
//
// 托盘图标 + 设置窗口（LingxiUI 进程内）。
// - 托盘左键：开/关设置窗口；右键：菜单（设置 / 拼音模式开关 / 退出）
// - 设置即时保存到 HKCU\Software\LingxiIme，DLL/UI 下一次读取即生效
//
#include <windows.h>

namespace lingxi {

// 创建隐藏主窗口 + 注册托盘图标（explorer 重启自动恢复）
void InitTrayAndSettings(HINSTANCE hInst);

} // namespace lingxi
