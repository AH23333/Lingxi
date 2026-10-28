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
    uint32_t candidateFontSize = 16; // 候选字号（12..32，按 DPI 缩放）
    uint32_t darkMode = 0;        // 0 跟随应用 1 浅色 2 深色
    bool enCompletion = true;     // 英文逐键补全（词中前缀替换）
    bool paused = false;          // 暂停预测：完全直通，不出任何候选（临时需要时用）
    bool cnPunct = true;          // 中文标点：中文模式下 . , / ; 等映射为全角标点
    bool fuzzyPinyin = true;      // 模糊音：z/zh、c/ch、s/sh、n/l、前/后鼻音互通（精确优先）
    uint32_t toggleMode = 1;      // 中英切换键（B5 草案②）：0 无 1 Shift 2 Shift 或
                                  // Ctrl 3 CapsLock（KeyDown 即吞，不翻转大小写）
                                  // 4 仅 Ctrl。
                                  // 无拼音时单击切换中英文（临时英文）并弹"中/英"提示。
                                  // 注册表双键兼容：ToggleMode（新，优先）+ ShiftToggle
                                  // （旧 bool，仍写——旧版 UI/DLL 只读它，两边语义一致）。
    bool vMode = true;            // v 模式：中文下首字母 v 直通英文
    bool shuangpin = false;       // 小鹤双拼：每音节两键（声母键+韵母键）
    bool symbolHotkey = false;   // 全局热键 Ctrl+Shift+B 调出符号面板（独占该组合键）。
                                  // 默认关：全局独占会占用应用自身快捷键（Edge 插件等
                                  // 实测冲突）；符号面板仍可从托盘/菜单打开。
    bool traditionalOutput = false; // 繁体输出：候选简体→繁体（字符级映射）
    bool llmRerank = false;        // LLM 拼音候选重排：默认关（打字期额外 CPU / 慢机自动降级）；
                                   // 想要上下文语序更自然再手动开启，设置页可勾选，实时生效
    bool llmMultiPath = false;     // LLM 多路采样（R3 §3）：贪心路跑完再采样路投票/合并候选。
                                   // 默认关：每次 LLM 调用约 +150ms CPU；seed=hash(prompt) 固定
                                   // 防候选闪烁；拼音重排只做 Borda 投票，top-1 不一致回退原序
    uint32_t skinIndex = 0;        // 候选窗配色：0 蓝 1 青 2 紫 3 绿 4 橙
};

Settings LoadSettings();
void SaveSettings(const Settings& s);

} // namespace lingxi
