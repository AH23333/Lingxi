#pragma once
//
// R6 屏蔽词（负面词典，GAP-VS-MSFT §8 R6）：存储 + 管理窗口。
// 所列词在候选生成阶段被整词剔除（引擎 Generate 输出过滤，零改动）。
// 存储：%APPDATA%\LingxiIme\blocked.txt（UTF-8 带 BOM，每行一词）；
// 引擎侧 LoadBlockedLocked 在 LoadUserDict 尾部调用（同 _udMtx 同通道），
// 保存后经 g_hooks.reloadUserDict 通知热载生效。
// 口径与引擎一致：词 1-50 字符、上限 500 条、BOM 容忍、去重。
//
#include <string>
#include <vector>
#include <windows.h>

namespace lingxi {

constexpr size_t kMaxBlockedLen = 50;      // 单词字符上限（引擎同口径）
constexpr size_t kMaxBlockedEntries = 500; // 条目上限（引擎同口径）

std::wstring BlockedWordsPathW();
std::vector<std::wstring> LoadBlockedWords();
bool SaveBlockedWords(const std::vector<std::wstring>& words);
void ShowBlockedWindow(HINSTANCE hInst, HWND hParent);

// 候选窗右键屏蔽入口（candwnd_interact.cpp 用）：内部完成去重追加+原子写+
// g_hooks.reloadUserDict 热载，调用方只管按结果给浮窗反馈。
enum class BlockWordResult
{
    Blocked,        // 已加入并热载
    AlreadyBlocked, // 已在屏蔽列表
    LimitReached,   // 已达 500 条上限
    TooLong,        // 空/超 50 字符（引擎同口径不装载）
    SaveFailed,     // 写入失败（文件被占用/无权限）
};
BlockWordResult BlockWord(const std::wstring& word);
bool UnblockWord(const std::wstring& word);  // 移除+热载；false=未找到或写失败
bool IsWordBlocked(const std::wstring& word);

} // namespace lingxi
