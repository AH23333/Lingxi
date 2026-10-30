#pragma once
//
// 托盘图标 + 设置窗口（LingxiUI 进程内）。
// - 托盘左键：开/关设置窗口；右键：菜单（设置 / 拼音模式开关 / 退出）
// - 设置即时保存到 HKCU\Software\LingxiIme，DLL/UI 下一次读取即生效
//
#include <windows.h>
#include <functional>
#include <string>

namespace lingxi {

// 创建隐藏主窗口 + 注册托盘图标（explorer 重启自动恢复）
void InitTrayAndSettings(HINSTANCE hInst);
// 打开设置窗口（托盘左键或命令行 --settings 调用）
void ShowSettingsWindow(HINSTANCE hInst);

// 注册"用户词典已变更（清空/导入）"回调，由 main 绑定到拼音引擎的 ReloadUserDict。
void SetUserDictReloadCallback(std::function<void()> cb);
// 注册"打开符号面板"回调，由 main 绑定（供托盘菜单调用）。
void SetShowSymbolsCallback(std::function<void()> cb);
// 注册"设置已保存"回调，由 main 绑定（用于同步热键等需随设置变化的状态）。
void SetSettingsChangedCallback(std::function<void()> cb);
// 注册"切换候选窗位置固定"回调，由 main 绑定到 CandWindow::TogglePin（托盘菜单勾选项）。
void SetPinToggleCallback(std::function<void()> cb);

// 候选词右键词典操作类型（候选窗右键菜单入口，2026-10-29 补齐遗忘需求）。
enum class CandDictOp
{
    Pin,    // 置顶：已有词条 +10000；未收录则按 count=10000 新增
    Boost,  // 提升：已有词条 +1000；未收录则按 count=1000 新增
    Lower,  // 降频：已有词条 -1000（下限 1）；未收录则失败
    Delete  // 删除：移除该词在用户词典中的全部词条；未收录则失败
};

// 该词是否已存在于用户词典（任一读音键）——右键菜单灰显"降频/删除"用。
bool UserDictHasWord(const std::wstring& word);

// 对候选词执行用户词典操作并通知引擎重载。
// keyHint：当前拼音组合串（推断拼音失败且非双拼时的键回退来源，可传空）。
// 返回 false=操作未生效（词条不存在/写入失败/无法确定键）。
bool ApplyCandidateDictOp(const std::wstring& word, const std::wstring& keyHint,
                          CandDictOp op);

// 推断汉字词的全拼键（小写连写；非汉字/推断失败返回空）——右键"查拼音"用。
std::wstring InferWordPinyin(const std::wstring& word);

} // namespace lingxi

// --diagnostics 全文（main.cpp 实现；打字统计窗"导出诊断报告"复用）
std::wstring BuildDiagnosticsText();
