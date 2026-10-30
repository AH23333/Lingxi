#pragma once
//
// R2 自定义短语（缩写→长句映射，GAP-VS-MSFT §8 R2）：存储 + 管理窗口。
// 打字时整串命中缩写即注入置顶候选（对标搜狗"自定义短语"）。
// 存储：%APPDATA%\LingxiIme\phrases.txt（UTF-8 带 BOM，行 = 缩写<TAB>短语）；
// 引擎侧 PinyinPredictor 只读同文件，保存后经 g_hooks.phrasesChanged
// 通知 main 触发引擎 ReloadUserDict（与用户词典同一热载通道）。
//
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace lingxi {

constexpr size_t kMaxAbbrevLen = 16;   // 缩写 2..16 位小写字母
constexpr size_t kMaxPhraseLen = 200;  // 短语长度上限（字符）
constexpr size_t kMaxPhraseEntries = 200; // 条目上限（文件规模控制）

struct PhraseEntry
{
    std::wstring abbrev; // 已归一小写
    std::wstring phrase;
};

std::vector<PhraseEntry> LoadPhraseEntries();
bool SavePhraseEntries(const std::vector<PhraseEntry>& entries);
void ShowPhraseWindow(HINSTANCE hInst, HWND hParent);

} // namespace lingxi
