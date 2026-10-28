#pragma once
//
// 快捷短语：用户自定义多组全局热键（须含 Ctrl 或 Alt），按下即在光标处
// 插入预设句式/句子。存储=%APPDATA%\LingxiIme\hotphrases.txt（UTF-8），
// 行格式：mods,vk<TAB>句子（mods/vk 十进制，mods 为 RegisterHotKey 位掩码，
// 不含 MOD_NOREPEAT——注册时统一追加）。
// 注册由 LingxiUI 主窗口完成（id 100 起）；本模块只管存储 + 管理窗口。
//
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lingxi {

struct HotPhrase
{
    uint32_t mods = 0;   // MOD_CONTROL / MOD_ALT / MOD_SHIFT 位掩码
    uint32_t vk = 0;     // 虚拟键码
    std::wstring phrase; // 插入的句式/句子
};

constexpr size_t kMaxHotPhrases = 24;

std::vector<HotPhrase> LoadHotPhrases(); // 缺文件/解析失败 → 空
bool SaveHotPhrases(const std::vector<HotPhrase>& entries);
std::wstring FormatHotkey(uint32_t mods, uint32_t vk); // "Ctrl + Alt + 1"

// 快捷短语管理窗口（单例；重复调用仅前置已建窗口）
void ShowHotPhraseWindow(HINSTANCE hInst, HWND hParent);
// 条目保存后回调（LingxiUI 重新注册全局热键）
void SetHotPhrasesChangedCallback(std::function<void()> cb);

} // namespace lingxi
