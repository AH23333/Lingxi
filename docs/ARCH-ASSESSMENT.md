# 架构与耦合评估报告

> 2026-10-31 · 回应用户主诉："耦合度过高，完全没有项目架构的观念"
> 方法：构建图实证（vcxproj 引用）+ include 扫描 + 代码度量，非主观印象。

## 一、结论先行

**项目有架构，且层级骨架是健康的；真正的耦合病灶集中在 ui 层内部的两个"上帝文件"上。**
适合重构，且是**低风险的"文件拆分 + 接口收口"级重构**，不需要、也不应该做架构级重写。

| 维度 | 评价 | 依据 |
|---|---|---|
| 层级结构 | ✅ 健康 | 三层构建图零违规（见下） |
| 接口抽象 | ✅ 健康 | IPredictor 统一 ngram/llm/pinyin + chain/reranker |
| 进程解耦 | ✅ 健康 | DLL↔宿主走命名管道，协议在 common/proto.h |
| ui 层文件内聚 | ❌ 病灶 | settingsui.cpp 2582 行 91 个文件域符号 5 个窗口类 |
| 全局状态 | ⚠️ 偏重 | 9+ 个 SetXxxCallback 全局回调 + 跨文件全局函数 |
| 测试安全网 | ✅ 极强 | engine_test 220 + deep_test + e2e + regress + probe 基线 |

## 二、实证：层级骨架（构建图）

```
common (静态库)          ← 无外部依赖
  ├── ime/ImeCore (TSF DLL)   ← 只 include common/*（扫描实证，零 ui 依赖）
  │       └── 命名管道 lingxi-ime-ipc ──┐
  │                                     │ 协议解耦：DLL 崩溃不连累宿主
  └── ui/LingxiUI (宿主 exe)    ← main/window/pipeserver/settingsui/
          usagestats/hotphraseui/phraseui/symbolpanel
          + engine/(ngram|llm|pinyin)   ← 引擎是 ui 的子层，接口 predictor.h
                tools/(engine_test|deep_test|e2e)   ← 各自独立 exe
```

- **ime 层零违规**：ImeCore.vcxproj 只编译 ime/*.cpp（5 文件）；全部 `#include "../common/*"`，无一条指向 ui。所谓"层级倒置"在当前构建图中**不成立**——DLL 通过管道拿预测结果，pinyin.cpp 编译进宿主而非 DLL。
- **engine 是干净的子层**：main.cpp 对引擎的全部依赖是 `#include "engine/predictor.h"` 一个头，三引擎可插拔。

## 三、实证：病灶在 ui 层内部

### 病灶 1：settingsui.cpp 上帝文件（2582 行）

一个文件聚合了 **5 个窗口类**（主窗/托盘、设置窗、帮助窗、统计窗、用户词典窗）+ 备份导入导出 + 自启动管理 + BuildDiagnosticsText + 全部设置页面（外观/候选/模糊音/皮肤/热键……）。91 个文件域符号。交互六~十单轮就 +991 行。恶果已经显形：

- 匿名命名空间内被迫前置声明 `ShowUsageStatsWindow`（定义在使用之后）；
- `BuildDiagnosticsText` 在 main.cpp 定义、settingsui.h 声明——**头文件是另一个文件的实现契约**；
- 每加一个窗（统计窗 466 行）都要在这一个文件里找锚点，改 A 窗时 diff 审阅必须趟过 B/C/D 窗。

### 病灶 2：window.cpp 候选窗（1388 行）

渲染 + 手势 + 工具条 + 翻页 + 皮肤 + 字体 + tooltip + 右键菜单 + 固钉拖拽——第二个上帝文件，同样在交互六~十膨胀最快。

### 病灶 3：全局回调网

`SetUserDictReloadCallback / SetPhrasesChangedCallback / SetSettingsChangedCallback / SetHotPhrasesChangedCallback / SetShowSymbolsCallback / SetPinToggleCallback / SetShowSymbolsCallback` + main.cpp 的 `g_reqSettings/g_reqSymbols/g_insertText/g_hotPhrases`。接线全部隐式，main.cpp 541-556 行一段"装配代码"的正确性依赖人工推理赋值与 CreateThread 的先后序。每加一条功能线就多 1-2 个全局 setter。

### 非病灶（澄清）

- ime 层、common 层、engine 子层：**不要动**。
- 跨文件符号可见性靠头文件节制（如 window.h 对 settingsui 只暴露需要的）基本合格。

## 四、是否适合重构：适合，且有强安全网

**论据：**
1. 病灶是**文件组织**问题，不是依赖方向问题——拆文件不动任何调用语义，回归可机器裁决（regress 三套件 + probe 基线 + 220 引擎用例，本轮刚全绿）。
2. 每个窗口类天然自带拆分边界（WndProc + Create + 私有全局），搬移即拆分，冲突面小。
3. 反论据（须诚实呈现）：当前功能演进速度极高（交互一~十，两天几十项功能），任何重构窗口都会被新功能挤占。**建议不要现在停功能做重构**，而是立规矩渐进：

## 五、分阶段方案（每阶段独立可验证、可中止）

- **P0 立规（零成本，即刻生效）**：新增窗口/功能一律新文件（usagestats 就是正确示范）；settingsui.cpp 只准减不准增。
- **P1 拆 settingsui.cpp**（下次触碰该文件的功能到来时顺手做，或专门一轮）：
  `tray.cpp`（主窗+托盘+热键）｜`settingswnd.cpp`（设置窗框架+页面模型）｜`helpwnd.cpp`｜`statswnd.cpp`｜`userdictwnd.cpp`｜`backup.cpp`。纯搬移，每拆一个跑一次 `regress.cmd` + smoke1/smoke2（tmp/ 已备好），BuildDiagnosticsText 随主窗归位。
- **P2 拆 window.cpp**：`candwnd_core`（窗口/消息/固钉）｜`candwnd_render`（皮肤/字体/绘制）｜`candwnd_interact`（手势/工具条/菜单）。
- **P3 回调收口**：9+ 个 setter 归并为一个 `UiHooks` 注册结构（settingsui.h 只暴露 `InitTrayAndSettings/ShowSettingsWindow` 级别的窄接口），main.cpp 装配段从 16 行散调变成 1 个结构体填充。
- **验收标准**：全程 regress ALL PASS + smoke 15+8 全绿 + probe 基线不回归；任一阶段失败可单独回滚（纯搬移无语义变更）。

## 六、给主诉的正面回答

"完全没有项目架构的观念"——**结论修正为：架构在（分层/接口/进程边界/测试金字塔都在且被执行），缺的是 ui 层的文件纪律**。这轮真机走查恰好证明：2582 行的 settingsui.cpp 里藏着的转发链、单实例、矩形记忆逻辑全部正确——问题不是"写得乱所以错"，而是"写得挤所以难审"。拆文件是体力活，安全网已就位，时机由功能节奏决定。
