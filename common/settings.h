#pragma once
//
// 用户设置（HKCU\Software\LingxiIme）。
// DLL 与 LingxiUI 共用；LoadSettings 带 SettingsSeq 版本号缓存（M5）：每次
// 调用仍单键读版本号（µs 级），SaveSettings/SaveSettingsDiff 落盘即 bump，
// 下一次读立刻反映改动；外部直写单键（reg 导入等）由 500ms TTL 兜底。
// 缺键 = 默认值。UI 保存、DLL/UI 各自读取。
//
#include <cstdint>
#include <string>

namespace lingxi {

struct Settings
{
    bool pinyinEnabled = true;    // 拼音组合模式（zh-CN profile 字母上屏汉字）
    uint32_t candidateCount = 9;  // 英文补全候选数（1..9）。注意：拼音候选不受此项控制——
                                  // 每页固定 kPageSize=9、总量 kMaxCandidates=60（common/proto.h），
                                  // 拼音翻页由数字键 1-9 对应页内位置（pipeserver.cpp Predict 分发处消费）。
    uint32_t candidateFontSize = 16; // 候选字号（12..32，按 DPI 缩放）
    uint32_t darkMode = 0;        // 0 跟随系统 1 浅色 2 深色（R7：跟随=宿主直读系统主题）
    bool enCompletion = true;     // 英文逐键补全（词中前缀替换）
    bool paused = false;          // 暂停预测：完全直通，不出任何候选（临时需要时用）
    bool incognito = false;       // 无痕模式（批次 G）：选词不学习/不计打字统计/不记
                                   // 最近符号与剪贴板历史（预测与收藏不受影响）；
                                   // 托盘开关，单键快读（IncognitoActive）即时生效
    bool cnPunct = true;          // 中文标点：中文模式下 . , / ; 等映射为全角标点
    bool fullWidthAlnum = false;  // 全角字母数字（搜狗 F4 对标）：非组合数字直出转全角０-９，
                                   // 原文上屏（回车/标点合并路径的无组合分支）整串转全角
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
    bool shuangpin = false;       // 双拼：每音节两键（声母键+韵母键）
    uint32_t shuangpinScheme = 0; // 双拼方案（shuangpin 开启时生效）：0 小鹤 1 自然码 2 微软
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
    uint32_t candOrientation = 0;    // 候选窗方向（C 横竖排切换）：0 竖排列表（默认，
                                     // 与历史逐像素一致） 1 横排单行条；横排放不下
                                     // （超工作区宽）本次渲染回退竖排（裁决 2026-11-03③）。
                                     // 非法值 clamp 回 0（照 CandidateCount clamp 先例）
    uint32_t decayEnabled = 1;       // B 词频衰减开关：0=score 恒等于 count（行为与
                                     // 衰减引入前一致），1=启用（裁决 2026-11-03①）
    uint32_t decayHalfLifeDays = 60; // B 半衰期天数（clamp [7,365]，缺省 60）：
                                     // score = count × 2^(−age/H)，查询时计算，
                                     // 原始 count 永不改写
                                   // 皮肤 0..6：1 青碧 2 橙 3 红 4 金 5 茶棕 6 玫粉
                                   // ——全 UI 统一换肤
    uint32_t candidatePinned = 0;  // 候选窗位置固定：0 跟随光标（默认） 1 固定在
                                   // candidatePinX/Y（候选窗"固"按钮/拖拽回显行或工具条设置）
    int candidatePinX = 0;         // 固定位置屏幕坐标（负值合法：多显示器左侧屏）
    int candidatePinY = 0;
    std::wstring recentSymbols;    // 符号面板"最近"页：\x1F 分隔、最新在前、≤12 项（面板维护）
    std::wstring favoriteSymbols;  // 符号面板"收藏"页：\x1F 分隔、最新在前、≤60 项（右键符号收藏，面板维护）
    uint32_t dailyCharGoal = 500;  // 每日上屏汉字目标（R8 统计增强）：达成当日一次性
                                   // 候选窗气泡提醒；0=关闭提醒。统计窗显示进度
    uint32_t lastSettingsPage = 0; // 设置窗：上次查看页（重开直达；越界回首页）
    uint32_t lastSymbolCat = 1;    // 符号面板：上次查看的静态分类标签（动态页不记忆）
    int setWinX = -1;              // 设置窗上次位置/尺寸（-1=未记录，创建时回居中默认）
    int setWinY = -1;
    int setWinW = 0;
    int setWinH = 0;

    // ===== 自助更新（docs/update-design-2026-11-03.md §1/§8）=====
    std::wstring updateRepo;                 // "owner/repo"；空=未配置更新源（正常态）
    uint32_t updateAutoCheck = 0;            // 1=启动后台检查（每天至多一次）；默认 0
    unsigned long long updateLastCheck = 0;  // 上次检查 Unix 秒（REG_QWORD）
};

Settings LoadSettings();
// 无痕模式快速查询：单键读（选词热路径用，避免全量 LoadSettings 的 ~30 项注册表读）
bool IncognitoActive();
// 测试派生快照（2026-11-01）：本进程是否带 LINGXI_SETTINGS_NS 派生。
// 背景：regress/e2e 设 LINGXI_SETTINGS_NS=test + APPDATA=沙箱，e2e 期间重生的
// LingxiUI 宿主带着两者活到测试结束后。main.cpp 生产路径清了 LINGXI_SETTINGS_NS
// （护设置键），但 clipboardui/autobackup 的惰性沙箱自检发生在清除之后，误判
// "非沙箱"，把剪贴板历史与自动备份写进 regress 临时 APPDATA——用户真实
// clipboard.dat/备份停更（实证：tmp\regress-*\userdata 下出现含探针的
// clipboard.dat，真实文件停在前一天）。修法：main.cpp 清除变量【前】先
// SnapshotTestSpawned() 固化判定；模块侧一律读 WasTestSpawned()，不再各自读环境。
bool SnapshotTestSpawned(); // 生产路径清除 LINGXI_SETTINGS_NS 前调用一次
bool WasTestSpawned();      // 任意时刻调用；未快照则惰性读当前环境
void SaveSettings(const Settings& s);
// 只写与当前落盘值不同的键（防"整页回写"把未同步/未加载的控件状态覆盖全部设置）
void SaveSettingsDiff(const Settings& s);
// 恢复默认：删除当前（沙箱感知）键下全部设置值，LoadSettings 随即回默认。
// 用户词典/热键短语在 APPDATA 文本文件，不受影响。
void ResetSettingsToDefaults();

} // namespace lingxi
