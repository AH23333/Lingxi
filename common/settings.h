#pragma once
//
// 用户设置（HKCU\Software\LingxiIme）。
// DLL 与 LingxiUI 共用；每次读取实时查注册表（键少、µs 级），改动即时生效。
// 缺键 = 默认值。UI 保存、DLL/UI 各自读取。
//
#include <cstdint>
#include <string>

namespace lingxi {

struct Settings
{
    bool pinyinEnabled = true;    // 拼音组合模式（zh-CN profile 字母上屏汉字）
    uint32_t candidateCount = 9;  // 候选上限（1..9，数字键 1-9 对应）
    uint32_t candidateFontSize = 16; // 候选字号（12..32，按 DPI 缩放）
    uint32_t darkMode = 0;        // 0 跟随系统 1 浅色 2 深色（R7：跟随=宿主直读系统主题）
    bool enCompletion = true;     // 英文逐键补全（词中前缀替换）
    bool paused = false;          // 暂停预测：完全直通，不出任何候选（临时需要时用）
    bool cnPunct = true;          // 中文标点：中文模式下 . , / ; 等映射为全角标点
    bool fuzzyPinyin = true;      // 模糊音总开关：关=全部组失效；开=按 fuzzyGroups 位掩码生效
    uint32_t fuzzyGroups = 0xFF;  // 模糊音组位掩码：bit0=zh/z bit1=ch/c bit2=sh/s bit3=n/l bit4=前后鼻音 bit5=f/h bit6=l/r bit7=g/k
                                  // （对标微软拼音"可细分开关"；0 视为仅总开关无组=无互通，默认全组）
    uint32_t toggleMode = 1;      // 中英切换键（B5 草案②）：0 无 1 Shift 2 Shift 或
                                  // Ctrl 3 CapsLock（KeyDown 即吞，不翻转大小写）
                                  // 4 仅 Ctrl。
                                  // 无拼音时单击切换中英文（临时英文）并弹"中/英"提示。
                                   // 注册表双键兼容：ToggleMode（新，优先）+ ShiftToggle
                                   // （旧 bool，仍写——旧版 UI/DLL 只读它，两边语义一致）。
                                   // （v 模式设置已退役：v 临时英文现为全拼默认行为，
                                   //   v 与普通字母一致走拼音组合，空格/回车原样上屏。）
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
    bool assocAfterPick = true;   // 选词联想（GAP §11）：中文全量上屏/联想点选上屏后自动弹联想
                                   // 候选（可空格连续输出，对标 MS）；任何无组合按键即撤销。默认开。
    uint32_t skinIndex = 0;        // 候选窗配色（ui/uistyle.h kSkins）：0 翠绿（默认）
    uint32_t uiFont = 0;           // 界面字体族（ui/uistyle.h kUiFontFaces）：
                                   // 0 微软雅黑（默认）1 宋体 2 等线 3 Segoe UI
    uint32_t candidateOpacity = 100; // 候选窗不透明度%（70/80/90/100），GAP §3 透明度定制
                                   // 皮肤 0..6：1 青碧 2 橙 3 红 4 金 5 茶棕 6 玫粉
                                   // ——全 UI 统一换肤
    uint32_t candidatePinned = 0;  // 候选窗位置固定：0 跟随光标（默认） 1 固定在
                                   // candidatePinX/Y（候选窗"固"按钮/拖拽回显行或工具条设置）
    int candidatePinX = 0;         // 固定位置屏幕坐标（负值合法：多显示器左侧屏）
    int candidatePinY = 0;
    std::wstring recentSymbols;    // 符号面板"最近"页：\x1F 分隔、最新在前、≤12 项（面板维护）
    uint32_t lastSettingsPage = 0; // 设置窗：上次查看页（重开直达；越界回首页）
    uint32_t lastSymbolCat = 1;    // 符号面板：上次查看的静态分类标签（动态页不记忆）
    int setWinX = -1;              // 设置窗上次位置/尺寸（-1=未记录，创建时回居中默认）
    int setWinY = -1;
    int setWinW = 0;
    int setWinH = 0;
};

Settings LoadSettings();
void SaveSettings(const Settings& s);
// 只写与当前落盘值不同的键（防"整页回写"把未同步/未加载的控件状态覆盖全部设置）
void SaveSettingsDiff(const Settings& s);
// 恢复默认：删除当前（沙箱感知）键下全部设置值，LoadSettings 随即回默认。
// 用户词典/热键短语在 APPDATA 文本文件，不受影响。
void ResetSettingsToDefaults();

} // namespace lingxi
