#pragma once
//
// 用户设置（HKCU\Software\LingxiIme）。
// DLL 与 LingxiUI 共用；每次读取实时查注册表（键少、µs 级），改动即时生效。
// 缺键 = 默认值。UI 保存、DLL/UI 各自读取。
//
#include <cstdint>

namespace lingxi {

struct Settings
{
    bool pinyinEnabled = true;    // 拼音组合模式（zh-CN profile 字母上屏汉字）
    uint32_t candidateCount = 9;  // 候选上限（1..9，数字键 1-9 对应）
    uint32_t darkMode = 0;        // 0 跟随应用 1 浅色 2 深色
    bool enCompletion = true;     // 英文逐键补全（词中前缀替换）
    bool paused = false;          // 暂停预测：完全直通，不出任何候选（临时需要时用）
    bool cnPunct = true;          // 中文标点：中文模式下 . , / ; 等映射为全角标点
    bool fuzzyPinyin = true;      // 模糊音：z/zh、c/ch、s/sh、n/l、前/后鼻音互通（精确优先）
    bool shiftToggle = true;      // 无拼音时单击 Shift 切换中/英文
    bool vMode = true;            // v 模式：中文下首字母 v 直通英文
    bool shuangpin =false;       // 小鹤双拼：每音节两键（声母键+韵母键）
    bool symbolHotkey = true;    // 全局热键 Ctrl+Shift+B 调出符号面板（独占该组合键）
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

} // namespace lingxi
