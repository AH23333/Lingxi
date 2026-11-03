#pragma once
//
// ===== 设置页"更新"区接线（批次 5，设计 §8）=====
// 控件由 settingsui.cpp 创建后交入（UpdateUiAttach）；本模块持有状态机与
// worker 线程（检查 / 下载+校验+暂存+提权发起），全程异步、UI 线程零阻塞（T11）。
// 结果经 WM_UPD_STATE / WM_UPD_PROGRESS 回投设置窗口渲染。
//
#include <windows.h>

#include <string>

#include "../common/settings.h"

namespace lingxi {

// worker → UI 回投消息（+30 已被 candcmd 占用，+10/+20/+21/+22 托盘/通知）。
constexpr UINT WM_UPD_STATE = WM_APP + 40;    // wParam=(int)State；lParam=std::wstring*（处理方 delete）
constexpr UINT WM_UPD_PROGRESS = WM_APP + 41; // wParam=百分比（-1=未知总量）；lParam=std::wstring* 阶段名

struct UpdateCtl {
    HWND repoEdit = nullptr;   // 更新源仓库输入框（owner/repo）
    HWND autoChk = nullptr;    // 自动检查药丸开关（g_toggleOn 语义，经回调读写）
    HWND verLab = nullptr;     // 当前版本行 desc（动态填充）
    HWND lastLab = nullptr;    // 上次检查行 desc（动态填充）
    HWND statusLab = nullptr;  // 状态行 desc（动态填充）
    HWND btnCheck = nullptr;   // [检查更新]
    HWND btnApply = nullptr;   // [下载并更新]（仅"发现新版"可用）
    HWND ownerWnd = nullptr;   // worker 回投目标（设置窗口）
    bool (*getToggle)(HWND) = nullptr;
    void (*setToggle)(HWND, bool) = nullptr;
};

void UpdateUiAttach(const UpdateCtl& ctl);   // 控件建好后注入并首刷
void UpdateUiSyncFromSettings(const Settings& s); // LoadToControls 链路内调
void UpdateUiApplyToSettings(Settings& s);        // SaveFromControls 链路内调
void UpdateUiOnCheckButton();                     // WM_COMMAND 转发
void UpdateUiOnApplyButton();
void UpdateUiHandleMsg(HWND hwnd, UINT msg, WPARAM w, LPARAM l); // WM_UPD_* 处理
void UpdateUiAutoCheckOnStartup();                // main.cpp：开关开且距上次≥24h 才后台查
void UpdateUiShutdown();                          // 置取消并短等 worker（WM_DESTROY）

} // namespace lingxi
