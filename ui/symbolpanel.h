#pragma once
//
// 符号面板：分类符号 / emoji 网格，点击经回调插入宿主。
// 窗口用 WS_EX_NOACTIVATE（不抢宿主焦点，保证 TSF GetFocus 仍指向宿主），
// WS_EX_TOPMOST 置顶，类似候选窗。
//
#include <windows.h>
#include <functional>
#include <string>

namespace lingxi {

// 切换式打开符号面板（已打开则关闭——热键/托盘重按即关）。onInsert 在点击
// 符号时由 UI 线程回调。关闭入口：× 按钮 / 右键 / 热键重按。
void ShowSymbolPanel(HINSTANCE hInst,
                     std::function<void(const std::wstring&)> onInsert);

} // namespace lingxi
