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
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

} // namespace lingxi
