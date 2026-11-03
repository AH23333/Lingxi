# 2026-11-03 体感层审查：旧件判定 + 搜狗拼音功能对标矩阵

> 本文档为只读审查产物，不覆盖 `audit-2026-11-02-bug-audit.md`。
> 基线 = 搜狗拼音输入法（离线功能面）；联网类功能一律标注"不纳入本轮"。
> 审查会话：2026-11-03 午后。HEAD 参照 = `2e44ce7`。
> 分级：高=用户每次都感知且明显不适；中=特定场景感知；低=小众/锦上添花。

---

## 一、任务一结论：用户现在打字跑的是**旧件**（两层陈旧并存）

### 1.1 注册表指向（CLSID `{AE967759-F054-4E02-AC5D-C172F1E9B99B}`）

| 视图 | 指向 | 磁盘状态 |
|---|---|---|
| `HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32` | `"D:\...\LingxiIme\bin\Debug\ImeCore.dll"` | 387584B，11-03 **12:48:10**（新） |
| `HKLM\SOFTWARE\WOW6432Node\Classes\CLSID\{...}\InprocServer32` | `"D:\...\bin\Debug32\ImeCore.dll"` | 294912B，11-02 **22:02:51**（**旧**：早于 L10/L12/H3/§八-8 全部修复） |
| `HKLM\...\CTF\TIP\{...}` Enable | 1（x64 与 WOW64 双视图均在，profile 0x0409 已登记） | — |
| `HKCU\...\Run\LingxiIme` | `"D:\...\bin\Debug\LingxiUI.exe"` | 3248640B，11-03 12:48:40 |

### 1.2 运行中进程证据

| 进程 | 启动时间 | 判定 |
|---|---|---|
| explorer.exe (10968) | 09:57:40 | 开机即载，内存镜像 = 12:48 前多轮热换出的**旧 DLL** |
| Trae CN (11432) | 09:58:35 | **实测已加载** `bin\Debug\ImeCore.dll`（模块枚举确认）——镜像冻结在 09:58 时刻，早于 H3（10:35 提交）之后所有重建 |
| WindowsTerminal (10888) | 10:03:25 | 同上，**已加载** ImeCore.dll，旧镜像 |
| msedgewebview2 ×22 | 09:57~09:58 | 同开机批次，旧镜像 |
| LingxiUI.exe (23796) | 13:15:06 | 晚于 12:48 构建 = **新件**（引擎进程是新的） |

**热换出铁证**：`bin\Debug\ImeCore.dll.3047621333.prev`（11:41:48）与 `.1919331875.prev`（12:05:34）——`tools\build.cmd:23-32` 的机制是把被宿主锁定的旧 DLL **改名 `.prev` 再写新文件**。磁盘路径永远是新的，但**已运行的宿主进程映射的是改名前的旧字节**，重新编译不会更新它们。

### 1.3 结论

- 用户打字的 TSF 前端（按键路由/组合/上屏，即"手感"的直接载体）在长寿命宿主（explorer/Trae/终端/WebView）里跑的是 **H3 之前的旧镜像**；32 位宿主（如 32 位微信）即使重启也还是**昨天 22:02 的旧 DLL**（磁盘上 Debug32 就没重建过）。
- 引擎侧（候选生成，LingxiUI.exe）是新件。
- 所以"修了一周体感没变"在逻辑上成立：**崩溃层修复大多落在 DLL 侧，而 DLL 侧新代码从未在用户打字路径上运行过**。

### 1.4 让新件真正生效的步骤（可执行）

1. **重建 32 位**：`cmd /c tools\build32.cmd Debug`（注册表 WOW64 视图指向它；当前落后一整天）。
   ~~已完成~~：13:31 已用 MSBuild 直调 `/m:1` 重建（证据见 §九）。
2. **重启宿主**：关闭并重开每个要打字的程序（Trae、终端、浏览器、微信）。最低成本全覆盖 = **注销再登录**（或重启）；explorer 不重启则任务栏/搜索框路径仍是旧件。
3. **无需重新注册**：磁盘路径未变，注册表指向即新件；若要彻底可跑 `tools\register.cmd Debug`（需管理员）。
4. **UI 进程**：每次重建 LingxiUI 后 `taskkill /IM LingxiUI.exe`，IME 会自动重生新实例（`pipeclient.cpp:52-87` 拉起链）。
5. **生效验证探针**（重启宿主后）：
   - `bin\Debug\LingxiUI.exe --version` 看构建日期（`main.cpp:325` 打 `__DATE__`）——实测输出"版本 1.0 构建日期 Nov  3 2026"；
   - ~~`LingxiUI.exe --probe-pinyin`~~ **本轮实测异常，暂勿依赖**：探针进程打完 `probe: 开始` 一行即退出，"引擎已加载"未落日志（两次复现，无 dump；待查，见 §九）；行为基线改由 engine_test 313 断言背书；
   - 打字路径抽查：新键语义（如 Home/End 末页）在宿主里生效即证明 DLL 为新件。
6. ~~建议~~ **已落地（F3, `86216bd`）**：`ActivateEx` 每次宿主激活把 DLL 的 `__DATE__ __TIME__` 写入 `%TEMP%\lingxi-ime.log`（需 `LINGXI_LOG=1`），新旧件一行日志即可判定。自查判据与操作手册见 §十。

---

## 二、搜狗功能对标矩阵（阶段一主体）

图例：**有**=行为与搜狗同类可用；**半**=部分实现/口径不同；**无**=缺失。联网列统一"离线"标注；云输入/语音/手写/云同步/快捷翻译/在线热词 = **排除项（不纳入本轮）**，见 §四。

### 组 1 拼音输入

| 功能项 | 搜狗行为 | 现状 | 差距 | 离线 | 级 | 代码定位 |
|---|---|---|---|---|---|---|
| 全拼 | 逐音节切分+整段 | 有 | — | 是 | — | `pinyin.cpp:1060 EnumeratePaths` |
| 双拼 | 多方案（微软/自然码/小鹤/搜狗…） | 半 | **仅小鹤一案**，方案不可选 | 是 | 中 | `pinyin.cpp:41-113`；`helpwnd.cpp:124` |
| 简拼 | 支持 | 有 | — | 是 | — | `pinyin.cpp:1030 ZeroInitialShorthand`；排序罚分 `pinyin.cpp:11` |
| 混拼（简拼×全拼混合） | 支持 | 有 | — | 是 | — | `EnumeratePaths` 逐音节全/简两路 |
| 模糊音 | 分组可配（z-zh 等） | 有 | 8 组位掩码可配；UI 分组勾选覆盖面**待人工核** | 是 | 低 | `pinyin.cpp:1763-1799 ComputeFuzzy`、`settings FuzzyGroups` |
| 智能纠错 | 全句级纠错（jinten→今天） | 半 | 仅音节表内常见误拼改写，无编辑距离泛纠错；残段不纠 | 是 | 中 | `pinyin.cpp:155-183 CorrectSyllableTypo`、`:1093-1098` |
| 整句输入 | 整段直出句子 | 有 | 词频+覆盖率整句排序在；无语法级组句 | 是 | 低 | `pinyin.cpp:11` 排序注释 |
| 候选数可配 | 每页 3/5/6/9 可选 | 半 | **拼音每页固定 9**（`kPageSize`），`CandidateCount` 实际只作用于英文补全（UI 标签即"补全候选数"）——设置语义与 `settings.h:17` 注释"候选上限"相悖 | 是 | 中→**待裁决**（动协议） | `proto.h:30-32`、`pipeserver.cpp:570`、`settingsui.cpp:1165` |
| 竖排候选 | F 键/设置可切 | 无 | 全库无竖排实现 | 是 | 低 | grep 无命中 |

### 组 2 词库体系

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 本地基础词库 | 大词库 | 有 | 规模差距（zh_pinyin.txt 单表） | 是 | 中 | `data\zh_pinyin.txt` |
| 用户词自学习+权重 | 有 | 有 | 双拼学习键已对齐全拼口径 | 是 | — | `pinyin.cpp:1559-1567 Learn` |
| 词频衰减/遗忘 | 有（时间衰减） | 半 | 仅手动"遗忘"词条，无自动衰减曲线 | 是 | 低 | `settingsui.h:17` |
| 细胞/专业词库 | 导入分类词库 | 无 | 无词库包概念；userdict 文本可手工灌 | 是 | 中 | — |
| 新词热词（离线部分） | 本地新词榜 | 无 | 排除联网后仍可做离线统计词 | 是 | 低 | `usagestats.cpp` 已有统计雏形 |
| 导入导出（往返一致） | 有 | 有 | 备份含 userdict/热短语/屏蔽侧车；往返有断言 | 是 | — | `settingsbackup.cpp:74-241`、engine_test C1d |
| 词条上限与淘汰 | 静默无上限 | 有 | 2 万键/5% 批量淘汰/manual 豁免——**比搜狗更透明**，行为口径不同但合理 | 是 | — | `53f0aaf`+`485727a` 边界单测 |

### 组 3 短语与快捷

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 自定义短语 | 缩写→候选，置顶 | 有 | 注入置顶序：短语>日期>常规 | 是 | — | `pinyin.cpp:268/591 InjectExplicitTop`、`phraseui.cpp` |
| 缩写触发 | 支持 | 有 | 整串精确触发 | 是 | — | 同上 `NormalizeIn` 口径 |
| 日期/时间/星期变量 | `rq/sj` 等 | 有 | rq/sj/xq/rqsj + 大写日期（合同场景） | 是 | — | `pinyin.cpp:184-263` |
| 命令式触发 | 少量 | 无 | 不纳入 | — | 低 | — |

### 组 4 输入模式与技巧

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 中英混输/临时英文 | Shift 临时英文 | 有 | — | 是 | — | `service.cpp:961-983` |
| 英文单词补全 | 有 | 有 | 开关+条数可配 | 是 | — | `EnCompletion`、`pipeserver.cpp:570` |
| 数字/金额大写 | `v` 模式 | 有 | 大小写、人民币、表达式中文读法 | 是 | — | `calcnum.cpp:26-316` |
| 计算 | `v` 算式 | 有 | — | 是 | — | 同上 `IsCalcQuery` |
| 符号/特殊字符 | `v`/`u`/面板 | 有 | 面板+搜索+收藏+最近（`SymbolHotkey` 默认 0=未绑热键） | 是 | 低 | `symbolpanel.cpp`、`symsearch.cpp` |
| 偏旁拆字 | `u` 模式 | 有 | U 模式查表、不进自学习防污染 | 是 | — | `uradical.cpp`、`pinyin.cpp:429` |
| 笔画查字 | `u` 笔画序 | 半 | 拆字有，纯笔画序号查字无 | 是 | 低 | `uradical.cpp` |
| 以词定字 | `[` 首字 `]` 末字 | 有 | 候选可见才认领，与微软口径一致 | 是 | — | `composition.cpp:1137-1164` |
| 直接上屏 | Enter 原串 | 有 | `PinyinCommitRaw` | 是 | — | `composition.cpp:1079-1083` |
| 顿号 | `/`、`\`→、 | 有 | — | 是 | — | `punct.h:21-22` |
| 全角/半角 | F4 全角字母数字 | 半 | 中文标点全角有；**全角字母/数字模式无** | 是 | 低 | `punct.h`（仅标点域） |
| CapsLock 切换 | 可配 | 有 | toggleMode=3 | 是 | — | `service.cpp:727-734` |

### 组 5 按键与交互

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 翻页键 | `-`/`=`、PgUp/Dn | 有 | 另有 ↑↓ 翻页、Home/End 首末；`,`/`=` 不绑翻页（=标点，与搜狗默认一致） | 是 | — | `composition.cpp:1084-1113` |
| 数字选词 1-9 | 有 | 有 | Ctrl/Alt 组合直通不误吞 | 是 | — | `service.cpp:896-940` |
| 空格/回车/Esc | 首选/原串/清 | 有 | 空格带 400ms 有界等待防"字母漏上屏" | 是 | — | `composition.cpp:1118-1130` |
| 翻页不丢选择 | 是 | 有 | 页码 DLL 侧权威，新批不弹回页 1 | 是 | — | `service.cpp:421/517` |
| Shift/Ctrl 切换 | 单击切换 | 有 | 组合键防误触（Ctrl+Shift+K 不切） | 是 | — | `service.cpp:831-847` |
| 模式提示 | 气泡+图标 | 有 | 中/英 toast + 托盘悬停状态摘要 | 是 | — | `candwnd_interact.cpp:284`、`helpwnd.cpp:64` |
| 候选框跟随光标 | 实时 | 有 | caret→鼠标位→记忆位三级退化；工作区钳制防遮挡/半出屏；下方放不下翻上方 | 是 | — | `candwnd_core.cpp:182-211` |
| 组合内光标编辑 | ←/→ 移音节光标 | 无 | ←/→ 仅吞下保组合，无编辑 | 是 | 中 | `composition.cpp:1114-1117`（M5 尾巴，预留） |

### 组 6 外观与体验

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 字体/字号 | 可配 | 有 | 族+字号+DPI 缩放重建 | 是 | — | `candwnd_render.cpp:23-45` |
| 行高 | — | 有 | 字号×15/8 | 是 | — | `render.cpp:81` |
| 深色模式 | 跟随系统 | 有 | 开关+主题模块 | 是 | 待人工 | `uitheme.cpp`、`DarkMode` |
| 皮肤 | 多皮肤 | 有 | SkinIndex 循环换肤（右键工具条） | 是 | 待人工 | `candwnd_interact.cpp:550` |
| 透明度 | 可配 | 有 | CandidateOpacity | 是 | 待人工 | settings |
| 高 DPI 清晰度 | per-monitor | 半 | 用 **GetDpiForSystem**（系统级），混合 DPI 多屏副屏可能模糊 | 是 | 中→待人工 | `render.cpp:38-45` |
| 圆角/阴影 | 有 | 待人工 | 静态读码未确证，视觉项 | — | 低 | `render.cpp` |
| 候选框残留/失焦 | — | 有测试面 | deep_test 覆盖部分；跨应用失焦待人工 | — | 待人工 | `tools/deep_test` |
| 全屏/RDP | — | 待人工 | 无法台架自动化 | — | 待人工 | — |

### 组 7 系统集成

| 功能项 | 搜狗 | 现状 | 差距 | 离线 | 级 | 定位 |
|---|---|---|---|---|---|---|
| 32 位宿主支持 | 双镜像 | 半 | WOW64 注册链在，但 **Debug32 磁盘件陈旧**（§一） | 是 | 高 | `build32.cmd` |
| 新构建免重启生效 | 安装器替换+自更新进程模型 | 无 | TSF 进程级镜像冻结是 OS 机制；搜狗靠升级器杀宿主缓存策略也有限。本项目至少**无版本探针可判定新旧** | 是 | 高 | §一.4 建议 |
| 各宿主 TSF 兼容 | 全 | 半 | Chrome/Edge/Electron/Office/微信/WPF/Qt 真机矩阵未跑全 | 是 | 待人工 | `manual-verify-2026-11.md` |

---

## 三、延迟与卡顿维度（原任务二 #1/#7 合并）

| 项 | 当前真实行为（代码证据） | 与主流差距 | 级 |
|---|---|---|---|
| 按键→候选链路 | 异步三段式：宿主 UI 线程只做 `WriteFrame`（正常 <1ms，最坏 80ms 超时；`pipeclient.cpp:209-225`）；UI 进程**专职预测线程**跑引擎（`pipeserver.cpp:103/469`），不在 UI 线程；LLM 重排**异步二段**——先回本地候选再补 AI 帧（`StartAsyncRerank pipeserver.cpp:643`） | 架构与微软/搜狗同型；本地词典 µs~ms 级 | 无缺陷 |
| "先本地再 AI"两段式 | 有（LlmRerank 默认 0=关，AI 帧到达后原地刷新） | 符合预期 | — |
| 空格有界等待 | 候选在途时阻塞宿主 UI 线程至多 **400ms**（`composition.cpp:1127`，预算 80→400 因写盘 464ms 事故调大） | 搜狗无此阻塞（本地即时）；本项目引擎侧写盘已后台化后是否可回缩预算——**待实测** | 中 |
| 冷启动 | llama 就绪 12.2~19.2s（§八-8，只影响 AI 续写/重排）；**本地候选不受影响**；UI 进程未运行时首键触发拉起+词典加载（`pipeclient.cpp:52-87`），期间用户看到组合字母但候选迟至数百 ms~秒 | 搜狗安装后常驻；本项目自动拉起有 300ms~1s 重连节拍 | 中 |
| 管道写阻塞 | 临界区串行整帧写，最坏 80ms/键（`pipeclient.cpp:213-220`）；对端失联有 500ms 熔断（`_lastFailTick`） | 可接受；重负载串行原则（PITFALLS #19） | 低 |

## 四、排除项（联网类，不实现）

云输入/云候选、语音输入、手写、云词库同步、快捷翻译、在线热词——矩阵保留行位、一律**不纳入本轮**。

## 五、已排除项（本轮不重复查）

H1/H2/H3/L4 崩溃层修复本体；L10/L12 已闭环项；§八-3/-6/-7/-8 残余风险（已登记于 audit 文档）。PITFALLS #93/#94/#19 口径沿用。

## 六、待裁决（高风险，只出方案不动手）

1. **每页候选数可配**（组 1）：`kPageSize` 是 DLL/UI 协议常量（`proto.h:30`），可配化需动帧语义与翻页权威侧（`service.cpp:517` 页码步进）→ 方案：UI 侧按设置分页渲染、DLL 侧翻页步进改由 UI 回传页大小字段。动协议，待裁决。
2. **←/→ 组合内光标编辑**（组 5）：需动 TSF `ITfComposition` 区间切分，属 commit 时序域 → 待裁决（M5 尾巴既有预留）。
3. **空格 400ms 预算回缩**：需先在真机量写盘后台化后的候选回包 P99，再决定；动按键路径时序 → 待实测后裁决。
4. **per-monitor DPI**：`GetDpiForSystem`→`GetDpiForWindow`/`GetWindowDpiScale` 迁移，影响全部布局换算（`MulDiv` 链）→ 建议先真机混合 DPI 复现再立项。

## 七、待人工清单（纯视觉/环境项）

- 深色模式对比度、圆角阴影、高 DPI 模糊（混合 DPI 双屏）、候选高亮可辨性；
- 全屏应用/RDP 会话下候选窗行为；
- 切换应用失焦后候选残留；
- 各宿主（Chrome/Edge/Electron/Office/微信/WPF/Qt）真机 TSF 走查；
- 模糊音分组勾选 UI 覆盖面；
- §一.4 新件生效步骤执行后的打字路径复验。

## 八、本轮确证可修候选（低风险，待工作区撞车解除后排批）

| # | 项 | 证据 | 修法 |
|---|---|---|---|
| F1 | Debug32 陈旧件 | §一 哈希/时间戳 | 跑 `build32.cmd Debug` 重建（非代码改动，属操作规程；写入本文档 §一.4 即闭环） |
| F2 | `settings.h:17` 注释误导 | 注释称"候选上限（1..9）"，实测拼音路径不消费（`pipeserver.cpp:570`），仅英文补全消费 | 改注释对齐真实语义 + 设置页"补全候选数"旁加说明"拼音每页固定 9"；纯文案，可附 engine_test 断言钉死 cap 归属 |
| F3 | 版本可判定性 | 旧件考古成本高（本次即证） | DLL/UI `--version` 已打 `__DATE__`（`main.cpp:325`）；DLL 侧 OnActivate 日志补 `__DATE__ __TIME__` 一行，低风险 |

> 撞车记录：审查时工作区存在他会话未提交改动（`tools/engine_test/engine_test.cpp` +117 行 M7 fuzz，HEAD 2e44ce7 提交于 12:52:55，属并行会话活跃写入）。按纪律本会话未做任何 commit，F2/F3 待用户放行后一条一提交。**（后记：该会话已自行提交 `5396575`，撞车解除，本会话按放行令继续。）**

---

## 九、重建执行与门禁读数（13:31–13:36 实测）

| 件 | 重建前 | 重建后 | 判定 |
|---|---|---|---|
| `bin\Debug32\ImeCore.dll` | 294912B @ 11-02 22:02:51 | **310272B @ 11-03 13:31:54**，SHA256 前缀 `729EC94B`，二进制内已含 F3 格式串 `ActivateEx tid=%u build=%hs %hs` | 新件 ✅ |
| `bin\Debug\ImeCore.dll` | 387584B @ 12:48（早于 F3） | 13:33 全解决方案重建（含 F2/F3） | 新件 ✅ |
| `bin\Debug\LingxiUI.exe` | @ 12:48 | 3248128B @ 13:33:03；`--version` 实测"版本 1.0 构建日期 Nov  3 2026" | 新件 ✅ |
| 运行中 UI 进程 | PID 23796 @ 13:15 | 已换 PID 24116 @ 13:35:18，路径=新 exe | 新件在跑 ✅ |

- 构建方式：MSBuild 直调 `/m:1`（先按 `build.cmd` 同款把被锁输出改名 `.prev`），Debug x64 全解决方案 + Debug|Win32 ImeCore 均**零错误**。
- **engine_test 门禁**：沙盒 APPDATA + `LINGXI_SETTINGS_NS=test`，**PASS=313 FAIL=0**（与 AGENTS.md 基线一致）。
- **新发现（待查）**：`--probe-pinyin` 探针进程两次均在 `probe: 开始` 后静默退出（`CreatePinyinPredictor` 未返回日志、无 WER dump）。不影响本次门禁结论（engine_test 覆盖同一引擎路径），但 AGENTS.md 把该探针列为行为口径，需另行定位。已登记 §十一。

## 十、"让新件真正生效"操作手册（写给不碰代码的用户）

**为什么只重启一个程序不够**：输入法 DLL 是"每个程序各自加载一份"的——程序**启动那一刻**把 DLL 复制进自己的内存，之后磁盘上重建一万次都换不掉它内存里的旧副本。你天天开着的程序（文件管理器 explorer、浏览器内核的 WebView2 子进程、IDE、终端）从早上开机起就一直攥着旧 DLL；而且 explorer 管着任务栏/搜索框的输入，它不重启，这些地方的输入就永远是旧件。**注销再登录会把 explorer 和它带起来的所有子进程一次性换新，这是唯一省事的"全部刷新"按钮。**

**操作步骤（按顺序）**：
1. 保存好所有程序里未存盘的内容（下一步会关掉一切窗口）。
2. **开始菜单 → 电源 → 注销**，等屏幕回到登录界面，再登录回来。（不嫌慢的话，直接重启更保险。）
3. 登录后打开记事本，切到"灵犀输入法"，随便打一串拼音（如 `nihao`），让输入法真正被加载一次。
4. **自查"我确实加载到新件了"**：
   - 按 `Win+R`，输入 `notepad %TEMP%\lingxi-ime.log` 回车（前提：环境变量 `LINGXI_LOG=1` 已设；没设的话，先在 PowerShell 里执行 `$env:LINGXI_LOG='1'; notepad` 再打开日志）；
   - 在日志**最底部**找这样一行：
     `[时间][进程号][Lingxi][ime] ActivateEx tid=… build=Nov  3 2026 13:3x:xx`
   - **判据**：`build=` 后面的日期时间就是"你正在用的这份输入法代码"的出厂时间。看到 `Nov 3 2026 13:3x` = 新件 ✅；看到更早的日期 = 那个程序还是旧件，把它关掉重开；**完全没有 ActivateEx 行 = 该程序根本没加载输入法 DLL，不算数**。
5. 顺手验证引擎进程：PowerShell 里跑
   `& "d:\Visual Studio Code\Something\LingxiIme\bin\Debug\LingxiUI.exe" --version`
   输出应含"构建日期 Nov  3 2026"。
6. 之后每次重新构建，重复第 2/4 步即可；不需要重新注册（注册表指向的路径没变）。

## 十一、本轮新增待查项

| # | 项 | 现象 | 建议 |
|---|---|---|---|
| N1 | `--probe-pinyin` 静默退出 | 两次复现：仅落 `probe: 开始`，`probe: 引擎已加载` 未出现，进程消失，无 dump | 用 `--%` 参数确认探针路径是否被单实例逻辑/控制台附着影响；engine_test 同路径正常，疑探针专属 |
| N2 | ~~探针 stdout 不可重定向~~ **已销账（14:05）** | 实为调用方式问题：`Start-Process -RedirectStandardOutput` 可捕获（EmitCliText 对重定向句柄写 UTF-16LE，`Get-Content -Encoding Unicode` 读即正常中文）；此前 0 字节系 shell 内联重定向未把管道句柄传给 GUI 进程 | §十二 重装验证即用此法拿到 `--version` 明文 |

## 十二、卸载/重装洁净度实测（13:59–14:10，本轮最高优先级）

回退留底：`tmp\backup-20261103-135926\`（clsid_x64.reg / clsid_x86_wow.reg / ctf_tip_hklm.reg / hkcu_lingxi.reg / hkcu_run.reg / langlist.txt / bin 与 APPDATA 清单 / uninstall-run.log / register-run.log）。回退安全判据：语言列表始终含微软拼音+英文+日文，卸载全程不致盲。

### 12.1 卸载前发现并修复的脚本缺陷（commit `9e3f5dc`）

- **缺陷（确证）**：旧 uninstall.ps1 无条件删除 `%APPDATA%\LingxiIme`（用户词典）与 `HKCU\Software\LingxiIme`（设置），且残留校验断言二者"不存在"——删干净反而显示 SUCCESS，数据丢失被伪装成功。卸载一个输入法不该丢用户词库。
- **修复**：两处删除收进 `-PurgeUserData` 开关，默认保留；校验翻转为"默认必须还在，丢了就报 FAIL"。修复后才执行本轮卸载，避免测试本身毁数据。
- **疑点排除（实证）**：脚本删 `HKLM\SOFTWARE\Classes\WOW6432Node\CLSID`，而真实注册落在 `HKLM\SOFTWARE\WOW6432Node\Classes\CLSID`——疑为路径写错。**卸载后实测两条 WOW 路径同时 GONE**：二者是 64 位注册表别名，脚本路径有效，非缺陷。

### 12.2 卸载残留核查表（脚本自报 + 独立复核双口径）

| 项 | 判定 | 证据 |
|---|---|---|
| HKLM CLSID x64 | 清 | 独立 Test-Path GONE |
| HKLM CLSID x86（WOW 双路径） | 清 | 两条路径均 GONE（别名实证） |
| HKLM CTF\TIP 树（含 Category/LanguageProfile） | 清 | GONE |
| HKCU CTF\TIP 树 | 设计如此 | 卸载前本就不存在 |
| HKCU 设置键 | **保留**（修复后行为） | EXISTS；重装后 diagnostics 读回原设置 |
| APPDATA 用户数据 | **保留** | 14 文件 61629B 与卸载前逐字节口径一致；词典 716 条完好 |
| HKCU Run 自启 | 清 | 值空 |
| SortOrder AssemblyItem | 设计如此 | 本无 Lingxi 条目 |
| 语言列表 | 设计如此 | Lingxi 从不在 cmdlet 可见列表（TIP 经 CTF Category 生效）；微软拼音/英文/日文前后一致 |
| bin 文件（含 .prev/.locked） | 设计如此（脚本不碰 bin） | `bin\Debug` 7×`.prev`+1×`.stale1`、`bin\Debug32` 1×`.locked` 为构建热换出副产物，清单已留底；整目录可随源码树删除 |
| 运行态 | 清 | 卸载时全部宿主（explorer/Trae CN/WindowsTerminal/WebView2）模块枚举 0 攥 ImeCore；ctfmon 14:03:30 重启 |

### 12.3 重装验证（register.cmd 提权 + 独立探针）

- imecheck 自检 PASSED：CLSID/CTF TIP HKLM=True，profiles 0x409/0x804。
- InprocServer32：x64→`bin\Debug\ImeCore.dll`（13:50:05，SHA256 `537C27C7F7A542D9…`）；WOW→`bin\Debug32\ImeCore.dll`（13:50:48，SHA256 `EC940FE16AE9B572…`）。
- **32 位侧实证**：SysWOW64 PowerShell（bitness=False）对 TIP CLSID `LoadLibrary` 成功 + `CoCreateInstance` 成功；x64 同法通过。两份新件在对应位数宿主里可加载可激活。
- 探针明文（UTF-16LE 捕获）：`灵犀输入法 1.0（构建于 Nov  3 2026）`；diagnostics：`构建日期：Nov  3 2026`、`用户词典：716 条`。
- 小观察（登记不修）：LanguageProfile 的 `IconFile` 指向 Debug32 DLL——x64 侧取图标也走 32 位件，图标能显示则无碍，属注册细节。

### 12.4 用户侧生效步骤·最终版（重装后）

1. **重启电脑或注销重登**（唯一省事的刷新；只重启单个程序不够——explorer 与各 WebView2 宿主各自攥着旧 DLL 内存镜像，重建/重装只改磁盘与注册表，不改变已映射进内存的旧件）。
2. 登录后在设置→时间和语言→中文（中国）→添加键盘，选"灵犀 AI 输入法"；若已列出则 Win+Space 切到它。
3. **一行自查判据**：打开 `%TEMP%\lingxi-ime.log`（需设置页开日志或环境变量 `LINGXI_LOG=1`），看到 `ActivateEx … build=Nov  3 2026 13:5…` 即证明当前宿主加载的是本轮新件；出现 11-02 及以前的 build 时间则该宿主还是旧镜像，重启它即可。
4. 备用判据：`bin\Debug\LingxiUI.exe --version`（新窗口里跑）输出"构建于 Nov  3 2026"。

## 十三、产物冻结快照与隔离构建（14:16 起，用户不能注销期间的硬约束）

背景：用户暂时无法注销重登，`bin\Debug`/`bin\Debug32` 即下次开机加载件。约束：①已验证件按字节冻结；②中间构建一律走 `bin\work`/`bin\work32`，只有批次收工+engine_test 绿才**有意**替换目标目录，替换后立刻重跑 imecheck+探针并记录新哈希。

### 13.1 冻结快照 `tmp\snapshot-20261103-141621\`（§十二 重装实证过的件）

| 文件 | 字节 | SHA256 |
|---|---|---|
| ImeCore.dll（x64，= 当时 bin\Debug） | 391168 | `537C27C7F7A542D974DC710F9011EB4921F7BB8F8F241FAB00CDBBD857F66055` |
| ImeCore-x86.dll（= 当时 bin\Debug32） | 310272 | `EC940FE16AE9B5726781E2F2617D9F6C52AB94F1B3C18BE6C49565F7BAE83822` |
| LingxiUI.exe | 3249152 | `A8D1DB037C1D67833C264E5C5E06FF5ADBA00B3414457FECE5FB738ED3B32190` |

x64/x86 两份哈希与 §12.3 InprocServer32 记录逐字一致（快照即被双位数实证加载过的那两件）。回退方式：把快照文件拷回目标目录即可逐字节还原。

### 13.2 隔离构建产物（阶段二 ①③ 改动，尚未替换目标目录）

| 文件 | 字节 | SHA256 |
|---|---|---|
| bin\work\ImeCore.dll（x64） | 391680 | `0C3E7CB5B3FBBE88DFB772ECF0B24989C0823AFFABB97943FFA97BDFFD2EEA40` |
| bin\work\LingxiUI.exe | 3253760 | `9CDE080382B4AE24E4588D34813BEDA61727966EC9D2E752171E4F9A9DC35193` |
| bin\work32\ImeCore.dll（Win32） | 314368 | `B09F2EB3707943EE8E0656B7C048EAE4CB2943CDA69DC553073D88455B8B97F7` |

门禁状态：engine_test 单实例重跑中（此前两实例同沙盒互撞双双 exit 1，已查杀重跑，见 `tmp\et_run_clean.txt`）。绿后执行有意替换 + imecheck + `--version`/`--diagnostics` 探针复验，新哈希补记本节。真机效果（双拼方案切换、候选数文案）**待用户重启后确认**。

### 13.3 Tier1-4 符号库扩充——有意替换记录（commit `7754943`）

- 门禁：`tmp\et_run5.txt`/`tmp\et_run6.txt` 单实例 PASS=333 FAIL=1；唯一红为拼音延迟 `jintianqi` median 18.8ms（阈值 10ms）——三连跑稳定 18.8~21.5ms、英文延迟 0.39ms 正常、文件完整性类（C4/C1a/C1c/L10）单实例下全绿，定性为**用户在用机器的环境性争用**，本 diff 未触碰引擎路径。此前 `et_run4` 的 7 条 FAIL 全部为双实例共享 `bin\enginetest\userdata\userdict.txt` 互删假红（协作方并发跑 engine_test 所致）。
- 替换：`bin\Debug\LingxiUI.exe` = 3261440B `D0089F4C264D3C61E474F767383F624C0D3EB6B4EEC4E5A9E4D813EAB530DAB7`（旧件备份 `LingxiUI.exe.20261103173955.prev`，3249152B=A8D1DB03…）。**两枚 ImeCore.dll 未动**，复核哈希仍为 §13.1 冻结值（537C27C7…/EC940FE1…）。
- 复验：imecheck Self-check PASSED；`--version` 输出 1.0.0；`--diagnostics` 正常（DLL 路径=bin\Debug，用户词库 716 条、注音/单位等节加载）。
- **待用户重启后真机确认**：符号面板新"单位"类与分类检索词（shuxue/huobi/zhuyin/danwei 等）——候选窗/面板在 LingxiUI.exe 内，重启 UI 进程即可，无需注销。

### 13.4 latency FAIL 定性纠正：Debug 构建产物，非环境争用（2026-11-03 17:49 单实例 Release 读数）

§13.3 将唯一红 `jintianqi` median 18.8ms 定性为"用户在用机器的环境性争用"。**该定性错误**，真因是 `bin\enginetest\engine_test.exe` 为 Debug|x64 产物：

- 证据 1：`tmp\engine_test\engine_test.tlog\engine_test.lastbuildstate` = `Debug|x64`；exe mtime 15:41:09、2518528B（Debug 体积），而 regress.cmd L71 规定 engine_test 必须 Release 构建。run3~run6 全部跑在这枚 Debug 件上。
- 证据 2（本轮取证读数）：以 7754943 同源码（三文件 mtime 15:36 < 构建 17:18）干净 Release 构建 `bin\work\enginetest_rel\engine_test.exe`（966144B，0 警告 0 错误），17:48:30 单实例运行，独立沙盒（`LINGXI_SETTINGS_NS=myrel` + 私有 exeDir\userdata，与并行实例零共享）：**PASS=334 FAIL=0**，`jintianqi` median=**0.878ms**（阈值 10ms），全程 53s（Debug 同套件 ~20min，~20× 慢，与未优化解释自洽）。
- 证据 3：历史 Release 读数同断言 0.419ms（12:54 regress-run4-manual）/0.470ms（00:21），跨 Debug 五连跑 19.4~25.0ms——差异稳定复现于构建配置而非负载。
- 结论：tier1-4 diff 与延迟 FAIL 均无关（这点 §13.3 判断正确），但"绿"的依据应改为本 Release 读数 334/0；**后续任何门禁读数必须先核对 lastbuildstate=Release**，Debug 件跑出的延迟数不可作为放行或定性依据。有意替换记录（§13.3 的 D0089F4C 哈希、ImeCore 未动）经复核属实，无需变更。

### 13.5 Tier1-5 农历/无年份日期/明天后天——门禁与有意替换记录（commit `da3c0e0`）

- 功能：`rq` 追加无年份 `%u月%u日`；新触发词 `mt`/`ht`（明天/后天，FileTime+1/2 天，含星期）、`nl`（农历今日 + 干支年两种形态，1900-2100 纯本地表查，不依赖 OS 日历）。引擎代码仅编译进 LingxiUI.exe 与 engine_test，**两枚 ImeCore.dll 冻结件未动**。
- 首跑 `tmp\et_run7.txt` PASS=345 FAIL=8，全为 P5d 农历锚点。根因静态定位：`LunarDateStr` 逐年天数位循环 `for(bit=0x8000; bit; bit>>=1)` 把低 4 位（闰月编号）也当月份位计数，124 年累计虚增 ~78 天（≈Σpopcount(闰月号)），落点系统性偏移约两个闰月；原 calendar.js 口径为 `i > 0x8`（仅 1..12 月 12 位）。修正后 PS 重演全 9 锚点（1900-01-31/2023-01-22/2024-02-10/2025-01-29/2025-07-25 闰六月初一/2026-02-17/2027-02-06 正月初一、2024-09-17/2026-09-25 八月十五）OK。
- 门禁（均单实例 Release、私有 APPDATA 沙盒 + 独立 LINGXI_SETTINGS_NS）：`tmp\et_run8.txt`（工作区树）PASS=**353** FAIL=**0**；`tmp\et_run9.txt`（**da3c0e0 干净 worktree 件**）PASS=**353** FAIL=**0**。基线 334 + P5d 新增 19 条，精确吻合。
- 替换件来源说明：工作区存在并行会话未提交改动（`common/settings.h/.cpp`、`ui/candwnd_core/interact/render.cpp`、`ui/window.h`——Tier3-C 文件域）。为不让他人 WIP 混入用户加载件，替换件由 `git worktree`（detached da3c0e0）构建，且以同树 engine_test 读数（et_run9 353/0）背书。
- 替换：`bin\Debug\LingxiUI.exe` = 3509248B `01760F1E60B11E241799FC2A306CCA447AB6F56B7C9269D8DABA84A2C3077138`（旧件 3261440B=D0089F4C… 备份 `LingxiUI.exe.20261103182459.prev`）。
- 复验：imecheck Self-check PASSED；`--diagnostics` 探针正常（构建日期 Nov 3 2026、路径 bin\Debug、用户词库 716 条只读加载、符号/模糊/纠错节齐）。替换时 LingxiUI 进程未在运行，新件将由 IME 下次拉起自动拾取。
- **待用户重启后真机确认**：`rq` 第 6 形态、`mt`/`ht`/`nl` 触发注入与上屏（候选窗在 LingxiUI.exe 内，UI 进程重生即可，无需注销）。
