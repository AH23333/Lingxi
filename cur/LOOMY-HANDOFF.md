# LingxiIme · Loomy 会话交接（2026-10-28 下午，额度收敛版）

> 接手必读顺序：本文件 → `cur/SHUTDOWN-HANDOFF.md`（豆包的历史交接，规则仍有效）→ `cur/QC-REPORT.md` 末尾我的会话记录。
> 我（Loomy）本轮 = 深度测试+修复+性能+用户体验整改，共 **9 个提交**（d7debbd 之后），全部各自过构建，终态 regress 全链 ALL PASS。

## 0. 一句话现状

工作区干净；`tools\regress.cmd` 一键回归（engine 142 断言 + fix_readings 数据闸门 + deep_test 含 T12/T13 + e2e）刚跑绿（`tmp\regress-21792913`）。**用户最新一批体验反馈已侦查、未动工**（见 §3 backlog，每节都写了设计草案，按额度逐条做，一条一提交）。

## 1. 本轮提交链（git log 现读为准）

| 提交 | 内容 |
|---|---|
| `7a23d66` | 切分路径分池截断（K_POS/K_FINAL 死代码修复）+ engine_test 沙盒与证人用例 |
| `238df97` | 提交键候选有界等待（≤80ms）+ 拼音模式检测激活竞态定时器自愈 |
| `4ca0218` | R3 重排 WIP 全落地（双上下文隔离 KV / abort 硬闸 / EMA 降级 / SEH 熔断 / rrSeq 连击合并 / **ClientCtx shared_ptr 修悬垂**）|
| `cccf1fd` | deep_test settle 取证诊断 + regress 一键三件套+沙盒 |
| `b68a379` | README/QC 文档同步 |
| `684d22a` | 重排默认关 + settle 400ms（实测打字 CPU 66%→3.5%，候选窗抖动 0）+ 设置页勾选 |
| `5235046` | **多音字读音系统性补全**：228 字/239 读音（fix_readings.py 数据交叉审计+幂等+`--check` 闸门进 regress）——"谁"打不出（shei）的类级根治 |
| `1d3080c` | 续写复读输入框内容：LLM StripPromptEcho（与上文尾重叠裁剪，k≥2 防误伤叠词）+ ngram 尾重合去重 |
| `edf1c3b` | **现代词层管线化**：`tools/modern_terms.txt`(176 词人工注音) + build_pinyin `inject_modern()` 只升不降 band 置顶；修"自动补全只能出自动不全"（补全 rank 27.8万畸形+整词缺失）|

关键实测数字：engine 断言 119→142 全绿；拼音每键候选往返 57ms 均值（31-94）；长串残段候选 27→60；多音 `--check` 0 缺口；重排在本机（prefill 291 t/s）EMA 自动降级=预期行为（专文 §2.4）。

## 2. 三级生效模型（向用户解释体验差异必须用这个口径）

1. **词典/LLM/链/设置项** = 跑在 `LingxiUI.exe`：改数据或 UI 代码后必须**杀掉并重启 LingxiUI.exe** 才生效（它是常驻进程）。
2. **键盘/组合/提交/翻页/模式切换** = 跑在宿主进程内的 ImeCore.dll：TSF 特性，**用户自己的应用重启**才吃新 DLL（README 已注）。用户报的很多"还没修好"其实是旧进程驻留。
3. HKCU\Software\LingxiIme 实时读——开关不用重启。
- ⚠️ 当前运行的 LingxiUI.exe 实例是 regress 沙盒环境（APPDATA 指向 tmp\regress-*\userdata）拉起的——**用户现在打字的学习词会写进沙盒目录**。下一轮第一件事：`taskkill /f /im LingxiUI.exe` 后用真实 `$env:APPDATA="C:\Users\34697\AppData\Roaming"` Start-Process `bin\Debug\LingxiUI.exe`。
- ⚠️ 本桌面会话曾修改 **HKCU 的 LlmRerank=0 / ShiftToggle(未动)**——Loomy shell 与用户共享此 HKCU。`LlmRerank` 之前显式为 1（旧默认开），现语义=默认关+用户可勾选。若用户反馈"重排开关失效"先查这个键。

## 3. 体验整改 backlog（用户原话 → 已侦查根因 → 设计草案）

### B1. 长串按数字选短词后，剩余拼音被清空（"白打"）——用户最痛，优先
- 根因：`composition.cpp PinyinCommitHanzi` 提交即 `_pinyin.clear()` 全清（~432/496 行）；数字/空格/鼠标统一走它。候选本身有"覆盖几个音节"的信息（引擎 `add(word,cost,span,nwords)`，1263/1305），**协议没传**。
- 草案（全加法，旧 DLL/旧 UI 双向兼容）：
  1. 引擎 `Prediction` 追加 `cover`（每候选 = 消耗的**原始 context 字符串前缀字符数**；在归一化与 raw 间做 `posOf` 回映！引擎内部 `in` 去撇号，DLL 的 `_pinyin` 带撇号——`Predict(context)` 的 context 就是 raw，归一化处记映射）。双拼模式 cover 一律 0（=保持现全清行为，别猜键数映射）。用户词典 boost 候选 cover=0（保守）。
  2. `MsgCands` 帧 strings 之后**尾部追加** `u16 cover×count`；旧 DLL 顺序读串到 count 即停，容忍尾巴；新 DLL 读不到数组→全 0。`kProtoVer` 不动。
  3. `service.cpp OnCandidates` 存 `_candCover`；提交时若 `0<cover<_pinyin 长度` **且 `_pinyin` 无撇号**（有撇号也退全清，避免映射歧义）→ 上屏词后 `_pinyin.substr(cover)` 留作新组合：非 inline 模式 `_pinyin=rest; ClearCandidates(); SendPinyinCtx()`；inline 模式用 `PinyinStart(rest[0])` + 逐字 `PinyinAppend` 重启组合（rest 已含撇号则退全清更稳）。
  4. `LearnRec` 存 cover；`ApplyLearn` 学习键改用**消耗前缀**（原来整条 raw 记到"自动"这种部分词=错位学习，顺带修掉）。
- 回归：engine_test 断言 `Predict("nihao")` 的"你"候选 cover==1、"你好"==5；deep_test T14：打 `nihao`→数字选"你"→ 组合余 `hao` 自动再预测→空格上屏"好"。

### B2. 看不到自己已输入的拼音（默认非 inline 无任何回显）
- 现状：`window.cpp` 只画候选+页码，无组合串；`CandWindow::Show(caret,cands,dark,ver)` 签名没有 text。
- 草案：Show 加 `const std::wstring& composing` 参数（**纯 UI 内部**，不走协议）；`PredictProc` 传 `e.pinyin ? e.text : L""`；`StartAsyncRerank` 捕获同样的 raw 传头（同 ver 不跳变）；`LayoutAndPosition` 高 +1 行、`OnPaint` 首行绘制 `composing + "："`（下划线风格/半透明底色区分候选）。
- 风险：窗口高度变化影响翻转定位（已有翻转逻辑）；deep_test 无窗体断言，靠人眼+截图。

### B3. 首字母 i/u/v 特殊拼音"键死"——与微软模式对齐
- 用户原话"输入 i、v、u 等特殊拼音会导致其他部分按键无法使用"。
- 现状侦查：词典里以 i/u/v 为音节的单字行**实测为 0**（15:4x node 全表扫描）→ 首键 i/u 切不出音节、候选空，组合卡栏的根因在这里；**v 首字母 = vMode 直通英文入口**（设计如此），用户预期若是"女/绿(lv)"不该被抢——需实测确认 vMode 的 Esc/backspace 退出通路。
- 草案：**先写复现场景脚本**（deep_test T15：分别首键 i/u/v 然后继续打 3 键、空格、标点、退格、Esc，记录每步 focused/候选）拿行为矩阵，再决定对齐方案：i→yi 音节兜底、u→wu、v 保持英语直通但确保 Esc/backspace 出得了。别先改代码。

### B4. 英文模式下 AI 提示词仍是中文 + 续写重复（中文侧 StripPromptEcho 已提交但用户未见好效）
- 根因A（新）：`ChainPredictor::Predict`——**非词中的英文上下文优先走 LLM**（llm.cpp ~395），qwen-0.5b 对英文续写吐中文。草案：链改「上下文尾部 1 字符是 ASCII 字母/数字 **且** 整段近端无 CJK → 词表引擎优先（其候选天然英文），LLM 仅兜底」；deep/e2e 加"英文上下文候选必须纯 ASCII"断言。
- 根因B（复读）：StripPromptEcho 只裁 overlap≥2 且 ≤40 字；用户场景可能是**输出=整段上文逐字复读+新内容**被 40 字上限放过、或 LLM 输出与 tail 有标点差异绕过。**下一轮先在真机 UI 复现**（probe `echo 今天天气不错` 看候选是否复述），必要时：重叠上限抬到 tail 全长 + 空白/标点归一后比较。
- 注意：用户测的是旧 UI 进程（§2），先重启 UI 再收口述反馈。

### B4+. 15:50 复勘（本会话末尾新取证，修前先读这条）
- ChainPredictor 代码现状快照已实读（链=词中词表优先，其余**一律先 LLM**→词表兜底；无语言闸、**无任何按键钩子代码**）。
- 用户确认**中文模式**续写仍复读 → StripPromptEcho 形态没盖全：①它只裁"输出头部×上文尾部重叠"，模型**整段复述上文**（输出与 tail 全等/输出开头与 tail 结尾只差标点）时裁完仍复读；②k≥2 阈值留缝。**修法**：升级为"输出含 tail 任意 ≥2 字子串即整条丢弃该候选"级别的复读过滤 + T16 真实复读率统计再定阈值；别再用"裁头"单手段。
- 新反馈（Edge 插件快捷键被吞）**15:52 已核实**：全局键占用只有 `main.cpp:205-207` 的 `RegisterHotKey(Ctrl+Shift+B, MOD_NOREPEAT)`，**默认开**（settings.h `symbolHotkey=true`）；DLL 侧 grep 证实**无** `SetWindowsHookEx`（不是钩子链问题）。归因步骤：Edge 里先关设置项复现；仍失效再查中文模式下 TestKeyDown 的 eaten 面。修复（下一轮小步做）：symbolHotkey **默认关**（对齐"不默认强加全局占用"的既定原则，类比 llmRerank）+ 设置页文案注明该组合键为系统级独占 + deep_test T17：符号热键开/关两态下 F5/Ctrl+Shift+B/Win 组合在组合态不被 DLL 吞的断言。

### B5. 中文切换只有 Win+Space 硬切——Shift 单击用户无感 + 自定义诉求（未动，见上）
- 现状：shiftToggle（Shift 单击）已实现且默认 true（OnKeyDown VK_SHIFT 单按→`_tempEnglish`+WM_APP_LX_MODE→`SendModeToggleToast`）；用户没感知到 = 要么没装上成功（老 DLL 驻留！§2），要么单击判定窗口太苛刻。
- 草案：①deep_test T16 断言"zh 模式+空组合→单击 Shift→再打字母直通且候选不弹"（现 T0 对照测过 toast，选路没测全）；②设置页把 `shiftToggle(bool)` 升级 `toggleMode(0无/1左Shift/2 Shift或Ctrl)`——`settings.h` 加字段 + `settings.cpp` 双读兼容（旧键仍写）；③全局自定义热键（注册表可配 Ctrl+Shift+Space）= R4 尾巴大项，别在收尾轮动。

### B6. 词库质量类
- fix_readings + modern_terms 是**仅有的两条改数据通道**，都不手改 `bin\*\data` 生成物副本（会被 build 覆盖/漂移，本轮实证过）。新词进 `tools/modern_terms.txt` 一行 → `build_pinyin --modern-only` 幂等生效 → engine 断言收口。
- 15:4x 实测取证：`神/身/深` **都在** shen 精确桶（各 1 行）→ 若用户反馈某字"出不了"，方向是①同音桶 rank 序偏后被截页，或②模糊音展开表，非缺数据；用 `py->Predict(L"shen")` dump 全桶+桶内序再归类，勿臆断补词。若确是同音截断问题，动刀处是引擎排序（`kPredictMaxCandidates`/桶序），不是数据。

## 4. 工程纪律（本轮实证过）

- **数据改完必须刷生成物**：`ui/engine/data` →（build.cmd PostBuild 复制）`bin\Debug\data` / `bin\Release\data` / `ui\bin\Debug\data`；engine_test 的 `bin\enginetest\data\zh_pinyin.txt` 只在**缺失时**才拷（EnsureWordlists），**手改源词典后要删它**否则断言用旧表。本轮"现代词层断言假红"就是踩这个坑。
- PowerShell 内嵌 node/cl 传中文或含引号 JS **必炸**（本轮 3 次复发）：一律落盘 .mjs/.py/.txt 或 Edit 工具直接改，消息文件用 node `writeFileSync`+`-F`。
- edit 工具参数名就是 `oldString/newString`（camelCase），报 invalid_type 按 schema 重发，别猜别名。
- 括号/注释大段 splice 后**必须整体编译**兜底（本轮把 engine_test 注释区打坏两次，最终编译器才是裁判）。
- 回归脚本 `tools\regress.cmd`：三件套+闸门+每套件前 taskkill 自有 LingxiUI；它拉的 UI 用沙盒 APPDATA（不污染真实词典）。日志留档 `tmp\regress-*`。
- e2e/deep 冷启动偶发一次 `typing pass-through`/settle 假红 = ctfmon 前台 TIP 竞态（重试壳会吞；attempt1 红 attempt2 绿属正常，别当回归修）。
- 提交：作者沿用 repo config `LingxiDev`（勿改 git config）；`git -c user.name=... commit -F <无BOM文件>`；小步提交，一条一个逻辑域，提交消息里带**根因+设计**不是流水账。
- `tools/__pycache__/` 进了 .gitignore；`deps/data-src/*.txt` 整类 ignore——**要入库的人工数据层一律放 `tools/`**。
- 探针留存：`tools/pf_ux.cpp`（假客户端体验探针：连打 CPU/RTT/同 ver 双帧计数）、`tools/fix_readings.py`、`tools/modern_terms.txt`；`tmp/` 里本轮脚本已清理，回归日志留档。

## 5. 下一轮开工命令

```bat
:: 0) 先把 UI 换回真实环境(§2 警告)
taskkill /f /im LingxiUI.exe & timeout /t 1
powershell "$env:APPDATA='C:\Users\34697\AppData\Roaming'; Start-Process '...\bin\Debug\LingxiUI.exe' -WindowStyle Hidden"
:: 1) 全链回归(3-5 分钟)
tools\regress.cmd
:: 2) 按 §3 从 B1(部分选词保留拼音) 开刀;每做完一项:
python tools\build_pinyin.py --modern-only   :: 若动了词典
tools\regress.cmd    :: 全绿才提交
```

—— Loomy，2026-10-28 15:xx（本轮额度见顶，状态全部固化于此文件，无口口相传的暗知识）
