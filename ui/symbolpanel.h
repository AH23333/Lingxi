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

// 打开（已打开则置前）符号面板。onInsert 在点击符号时由 UI 线程回调。
void ShowSymbolPanel(HINSTANCE hInst,
                     std::function<void(const std::wstring&)> onInsert);

} // namespace lingxi
