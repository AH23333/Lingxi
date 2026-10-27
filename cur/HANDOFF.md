# 灵犀输入法 — 双代理协作交接说明

> 本文件由**开发代理（豆包）**维护，供**质检代理（GLM）**接入时阅读。
> 目的：对齐现状、避免改同一批文件冲突、明确铁律。

## 一、铁律（双方都不可违反）

1. **严禁** `NtSuspendProcess` / 暂停 / 重启任何**用户进程**（Edge、豆包、编辑器、资源管理器、另一个代理等）。早期一次误暂停导致 Edge 悬挂组合、用户无法输入中文，是本项目最严重事故。
2. 只允许重启项目自己的 `LingxiUI.exe`。
3. 宿主进程（Edge/豆包等）的重启**只能由用户本人操作**，助手不得代劳。
4. 改 HKLM / 注册 TSF 需 UAC；改完必须由用户重启宿主才生效。

## 二、当前状态（已对齐到提交）

- 最新提交：`7699412 符号面板完整落地 + 单实例命令转发 + 修复 ReadFrame 丢帧`
- 工作区：**干净**，无未提交改动。
- 测试：engine_test **0 FAIL**（Release，全 PASS）。
- 架构：双进程。`ImeCore.dll`（TSF 文本服务，注入宿主）+ `LingxiUI.exe`（候选窗+引擎，命名管道 `\\.\pipe\lingxi-ipc`）。
- **默认无内联组合模式**（`_inlineComp=false`），从架构上杜绝 Chromium 悬挂组合污染。

### 符号面板已完成（GLM 审查意见全部处理）
- 5 分类符号网格（常用/数学/货币/箭头/表情），点击经 MSG_INSERT_TEXT 插入宿主。
- 已用 fakeclient 验证完整闭环（点击"，"→ 客户端收到 MSG_INSERT_TEXT，见 PITFALLS #21~#24）。
- `cur/GLM_REVIEW_symbolpanel.md` 的 P1-1（emoji 代理对）、P1-2（selfInsert 守卫）、P1-3（诊断清理）及 P2（字体复用/多显示器/删死代码）**全部处理**。
- 同时修复了真实的 ReadFrame 丢帧 bug（连发 HELLO+ACTIVATE 第二帧被吞）。

## 三、本会话已修复的两个"烂摊子"（质检请重点回归）

### 1. 设置窗口创建时污染注册表（已修复）
- 现象：打开设置窗口时，子控件 Edit 带初始文本在 `CreateWindowExW` 期间触发 `EN_CHANGE → SaveFromControls`，此刻复选框全是未初始化的未勾选状态，导致把 `HKCU\Software\LingxiIme` 全部设置写成 0（拼音/补全/标点/模糊音/Shift/v 全关）。
- 修复：`CreateSettingsWindow` 在创建主窗口前置 `g_updating=true`，整个控件创建期间屏蔽保存，`LoadToControls` 结束才复位。
- 污染样本备份：`tmp\lingxi_reg_polluted.reg`（仅留档，勿重新导入）。

### 2. `LINGXI_LOG=1` 遗留环境变量导致性能暴跌（已清理）—— 性能"回归"的真正根因
- 现象：engine_test 拼音延迟从基线 0.8ms 暴涨到 15~44ms，且线程提权、关模糊音都无效。
- 根因：**用户级环境变量 `LINGXI_LOG=1`**（早期调试遗留）使 `InitLogging()` 开启 verbose 文件日志，`Predict` 里逐路径/逐候选的 `LX_LOGV` 每次都同步写文件。
- 已清理：删除用户级 `LINGXI_LOG`；删除 27.8MB 的遗留日志 `%TEMP%\lingxi-ime.log`。
- 注意：当前已运行的父进程（含工具的 PowerShell、可能的旧宿主）仍可能在进程环境里带 `LINGXI_LOG=1`。**新启动的进程已干净**；跑测试前若发现延迟异常，先在同一进程 `Remove-Item Env:LINGXI_LOG` 再跑。

## 四、文件分工建议（避免冲突）

近期高频改动、冲突风险高的文件：
- `ui/settingsui.cpp` / `ui/settingsui.h`
- `ui/engine/pinyin.cpp` / `ui/engine/predictor.h`
- `tools/engine_test/engine_test.cpp`

**建议分工（可协商后在此更新）**：
- **开发代理（豆包）**：继续功能开发（符号面板、自定义短语、双拼、皮肤、逐字新词学习等），主要改 `ui/`、`ime/`。
- **质检代理（GLM）**：以**只读分析 + 出具报告**为主；若要改代码，请先在本目录新建 `GLM_FIX_*.md` 说明要改的文件与方案，避免与开发代理同时编辑同一文件。
- 任一方准备改一个文件前，先 `git status` 看是否干净、看本目录是否有对方的新留言。

## 五、待办（按优先级）

1. 用户明确指示：**不要穷举同音词（预选词）**，先把非选词相关交互做好。符号面板已完成；下一步候选：自定义短语、双拼、字体字号皮肤、面板全局快捷键、候选窗 hover/失焦、逐字组合新词学习。
2. 符号面板增强：emoji 彩色（DirectWrite）、hover 高亮、失焦自动关闭。
3. 工程化：卸载清理验证、32 位 TIP、崩溃保护。
4. Edge/豆包兼容：隔离性 + ReadFrame 修复已就绪，待用户重启宿主复测。

## 六、常用命令

- 构建：`tools\build.cmd Debug`
- 跑测试（勿带 LINGXI_LOG）：`& ".\bin\enginetest\engine_test.exe"`
- Release 重建测试：`msbuild tools\engine_test\engine_test.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64`
- 打开设置：`Start-Process .\bin\Debug\LingxiUI.exe -ArgumentList "--settings"`（**勿加 -WindowStyle Hidden**，否则窗口不可见）
- 打开符号面板：`Start-Process .\bin\Debug\LingxiUI.exe -ArgumentList "--symbols"`（已有实例时经命名事件转发）
- 设置注册表：`HKCU\Software\LingxiIme`（删除该键=全默认）
- 用户词典：`%APPDATA%\LingxiIme\userdict.txt`（UTF-8 无 BOM）
