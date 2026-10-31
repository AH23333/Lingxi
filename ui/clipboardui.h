#pragma once
//
// 剪贴板历史（30+ 清单批次 C）：自动记录全局复制的文本，Ctrl+Alt+V 或托盘
// 菜单打开历史窗，双击/"复制选中"写回剪贴板。
// 自包含模块（autobackup/usagestats 同模式：自己的文件 IO 与监听窗口，
// 不依赖 uiinternal.h）。存储：%APPDATA%\LingxiIme\clipboard.dat（私有二进制
// 格式，原子写 .tmp+MoveFileExW）；上限 50 条、单条 ≤2000 字符、去重
// （重复复制前移不重复记）。沙箱宿主（LINGXI_SETTINGS_NS 非空）监听与
// 窗口全部停用，防回归测试污染真实历史。
//
#include <string>
#include <windows.h>

namespace lingxi {

constexpr size_t kCbMaxEntries = 50;     // 历史条目上限
constexpr size_t kCbMaxItemChars = 2000; // 单条字符上限（超出截断）

// 启动时调用一次（tray InitTrayAndSettings）：创建隐藏监听窗 +
// AddClipboardFormatListener；沙箱返回 false。此后全局复制自动入史。
bool InitClipboardCapture(HINSTANCE hInst);

// 历史窗入口（托盘菜单 2021 / 全局热键 Ctrl+Alt+V）。
void ShowClipboardWindow(HINSTANCE hInst, HWND hParent);

// 退出/注销前落盘（节流计数内未写的捕获补写；无脏数据为空操作）。
void ClipboardFlush();

} // namespace lingxi
