# 进度看板（PROGRESS）

> 一页看清：完成了什么、正在做什么、下一步。详细过程见 [DEVLOG.md](DEVLOG.md)，坑见 [PITFALLS.md](PITFALLS.md)。

## 里程碑

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M0 脚手架 | ✅ 完成 | 目录/构建（MSBuild 零额外安装）/GUID/日志/IPC 协议 |
| M1 TSF 骨架 | ✅ 完成 | 注册（双语言 profile）、激活、按键直通、全链路日志 |
| M2 候选窗+上屏 | ✅ 完成 | 跟随光标、鼠标/键盘选词、插入与前缀替换两种上屏 |
| e2e 自动化测试 | ✅ 完成 | 合成 ITextStoreACP 宿主，15 项断言；抗干扰重试改造中 |
| M3 真词表引擎 | ✅ 完成 | 英文 g10k 前缀补全 + 中文 jieba 高频接续词（8 万词条） |
| M4 本地 LLM 引擎 | ✅ 完成 | llama.cpp b11900 动态加载 + Qwen2.5-0.5B Q4 + 上下文感知降级链 + 异步预测管线；探针实证工作正常。详见 [HANDOFF.md](HANDOFF.md) |
| M4.5 KV cache 增量 | ✅ 完成（`8b5ed3b`，PROGRESS 旧行过时） | 前缀复用+退格回退：Generate/DoRerank 公共前缀计算 + seq_rm 分叉截断 + 完全相同重算末位（`ui/engine/llm.cpp`）；重排槽独立 _rrCtx 物理隔离 |
| M5 拼音输入 | 🔶 默认已启用，真实应用激活已修复 | 组合窗口 + 拼音引擎（engine_test 9/9，延迟 0.32ms）+ 双 profile 双模式。真实应用拼音不激活根因已修（HKL 判据无效 → GetActiveLanguageProfile 真值 + 消息泵空闲点检测，见 PITFALLS #17/#18/#18b）；候选 5→9；光标矩形空值兑底。待用户实测确认 |
| M6 打磨 | 🔶 设置+符号面板已上线 | 托盘图标 + 设置窗口（拼音模式/英文补全/候选数量/明暗/中文标点/模糊音/Shift 切换/v 模式/双拼/繁体输出/热键开关 + 用户词典管理，HKCU 存储，即时生效）；符号面板（5 分类符号/emoji，点击 IPC 插入，emoji 代理对，托盘入口 + Ctrl+Shift+B 可开关热键）；单实例命令转发；**用户六项需求 A/D/B(含E)/C ✅（晚·十）**：自定义中英切换键五态（含 CapsLock）、快捷键句库（全局热键上屏，DLL 零改动）、自定义候选词（手动添加+拼音自动推断+提升/降低排序）、设置页流式重排+纵向滚动+可缩放美化。**皮肤 ✅（晚·十二 `bc7be9e`）**：全应用 WinUI 化——uistyle.h 共享设计令牌（中性灰阶+非蓝紫五皮肤：翠绿/青碧/橙/红/金）+DWM 圆角+设置页白卡 owner-draw+符号面板药丸浮层+词典窗/快捷短语窗统一（视觉待人工确认）。**卸载清理 ✅（R5 轮）**：unregister.cmd 双位数 + `--purge`（HKCU 设置 + APPDATA 用户词典 + 临时日志）+ 残留自检。代码签名不可行（无证书；SmartScreen/杀软告警为已知限制）。M6 功能面收官 |
| 用户六项需求（A-F） | ✅ 6/6 完成 | A 切换键 ✅（`7a28923`）/ D 快捷短语 ✅（`7065b2a`）/ B+E 词典增强 ✅（`ebf0ae1`）/ C 设置页 ✅（`7bf02f5`）/ **F AI 预测 ✅ = R3 全量落地（核心前几轮分批 + 多路采样 `b3bad05`，见 R3 行）**；deep_test T16 加固 ✅（`c00e972` 免疫环境污染） |
| R1-R4 拼音体验 | ✅ 完成 | 词库 8 万→30 万 + 高频口语覆盖层；Viterbi 整句分词 + 末字变体；四层排序；模糊音；中文标点；候选翻页（PageUp/PageDown 与 -/=）；v 模式直通英文；Shift 单击中英切换；自学习用户词典（选字反馈 + 管理界面导入导出 + 新词入索引 + 简拼缩写调出）；小鹤双拼；繁体输出（OpenCC 映射）；符号面板；单实例命令转发；候选窗 hover 高亮；**B1 部分提交**（候选携带 cover=消耗的拼音前缀，选短词后剩余拼音留组合，学习键=消耗前缀根治错位学习）；**B6 拼音隔音符**（晚·十三：组合中 ' 切分边界 xi'an→西安；MatchWords 音节数校验仅显式隔音时生效（晚·十四 strictSep 修正：无隔音串级匹配是合法读法）；NormalizeIn 弯撇号 UTF-8 序列探测；晚·十四·续：PinyinKeyEaten 显式 case 吃撇号——此前依赖 cnPunct 标点映射间接吃键，cnPunct=false 时穿透宿主死码；deep_test T17 端到端 xi'an 空格提交西安 29/29 PASS）；**B7 候选窗滚轮翻页**（晚·十三：WM_MOUSEWHEEL ±kPageSize）；**组合期导航键接管**（晚·十四：Up/Down/Home/End/Left/Right 归 IME——翻页/首末页/吞下保守组合，防宿主光标移出组合范围错位）。engine_test 176 项断言全 PASS（含多音/现代词/分池证人/变体质量闸；词库+设置双沙盒） |
| 深度体验整改（Loomy backlog） | ✅ B1/B8/B4/B3/B5/B2 全部完成 | B1 部分提交全链路 ✅（b7d0ee1+f84f049，cover 协议）；B8 符号热键默认关 ✅（06548a8）；B4 英文上下文词表优先+LLM 候选纯 ASCII 过滤+复读裁剪升级 ✅（2b3b477，ChainPredictor/StripContinuationEcho 移入 ngram.cpp 供单测，engine_test +14 断言，probe-chain 真机取证）；B3 零声母 i/u 键死 ✅（ZeroInitialShorthand 20 项表 DP 内扩展 + SyllUnit.rawLen cover 回放，engine_test P6d +8 断言，deep_test T15 10 Report 行为矩阵；同轮根治测试基建楔死：程序化激活换 ProfileMgr API，PITFALLS #32）；B5 Shift 单击切换断言 ✅（deep_test T16 四断言 + TapKey/RawKeyUp 辅助；顺带修组合中 VK_SHIFT 落 unmapped-default 误提交首选，分发层单列放行；flaky 归因方法论 PITFALLS #33）；B2 候选窗拼音回显 ✅（Show 加 composing 参数纯 UI 内部，DLL 零改动；PredictProc 传组合串，重排线程同 ver 穿透；高度 +1 行，hover/点击行判定偏移，GDI 实色近似半透明区分；窗体验证像素取证确认——探针 MSG_CTX 驱动真实窗出窗+特征色分析，pinyin 有回显带/en 对照无且候选区下推 42 行因果实锤）；B5 草案② toggleMode 设置升级 ✅（shiftToggle(bool)→toggleMode 0无/1 Shift/2 Shift或Ctrl；settings 双读兼容+双写旧版无感；DLL 切换键状态机扩展 Ctrl+交错保护；设置页三态下拉；regress ALL PASS 派生路径端到端实证）。**backlog 收官**（详见 cur/LOOMY-HANDOFF.md）
| 体验投诉处置 + 自走查轮（Loomy 10-29） | ✅ 完成 | 五投诉（黑边/残影/遮屏/频闪/复读堆叠）+ 自走查四修（变体质量闸/回显掐头保尾/构建戳门控/设置回写双保险 loaded 门+SaveSettingsDiff）+ 转发观测日志；engine 142→176；PITFALLS #44/#45 立案；README 全量重写。待验证两项见 cur/LOOMY-HANDOFF.md 快照 | |
| AI 补全去重（10-29 下午 `d5a7d64`） | ✅ 完成 | 用户投诉"补全词大量重复"三种形态全灭：--probe-typing 取证（归一相等对/前缀堆叠对/0.5B 复读上文+词表占位垃圾）→ DedupCompletions 三重去重（归一相等保首/前缀保较短≥2 字/上文二元组重叠≥60% 剔，llm.cpp Generate 统一过闸）+ WordEngine::PredictChinese 占位退役（中文续写 LLM 独占）；engine_test E8 矩阵 9 例+占位退役断言（186 项全绿）；deep_test T14 改扫描式选词（序 pin 双重状态依赖根治）；e2e 中文 Tab 预算 12s（LLM 冷启动）；regress ALL PASS | |
| R3 LLM 整句重排 | ✅ 核心已落地（晚·十前分批）+ 多路采样收官（晚·十一 `b3bad05`） | 方案 cur/GLM_EXPLORATION_R3_LLM.md 分批落地：DLL 补发上文（`e328a7d` CTX_F_CTX_PREFIX+committed）→ 双上下文 KV 物理隔离/prefill abort 500ms 硬闸/EMA 吞吐自动降级/SEH 熔断（`4ca0218`）→ 重排默认关（`684d22a`）→ **多路采样 `b3bad05`**（贪心+采样双路：续写 fork 合并候选 seed=hash(prompt) 防闪烁；拼音重排 Borda 投票，top-1 不一致回退原序；llmMultiPath HKCU 默认关，设置页开关；采样链符号全可选缺则禁用）。旧文"未实施"系过时定性 |
| 词库数据管线 | ✅ 完成（2026-10-28 下午） | fix_readings 多音补全（228 字/239 读音，--check 挂 regress 闸）+ modern_terms 现代词层（176 词，inject_modern 只升不降，--modern-only 幂等）；PITFALLS #25/#26 |
| R5 工程化 | ✅ 全部完成 | **32 位 TIP ✅（`88b9252`）**：Win32 配置输出 bin\<C>32\、SysWOW64 注册进 WOW6432Node 与 x64 共存、EnsureUIRunning 回退链真机实证、tools\build32.cmd+register.cmd 扩展+host32.cpp 验证探针留存、真机三环取证（加载 dir32=1/管道已连接/杀 UI 自愈拉起）；WER LocalDumps 本机已配置并实证（DumpType=2 全量/保留 5 份）；SignalNamedEvent 4s 静默失败 P2 已修；卸载 unregister.cmd 已增强（双位数/--purge/残留自检）；regress.cmd 一键回归常备。见 cur/GLM_EXPLORATION_R2R5.md |

## 已验证的能力（e2e 自动化证明）

- 按键直通：激活后正常打字零干扰
- 候选窗：跟随光标真实显示、深浅色自适应、点击不抢焦点
- 英文前缀替换：`mor` → `more`，`inte` → `international`（Tab 上屏）
- 上屏后继续直通、多应用多实例管道、焦点切换后预测不失效
- 中文上下文续写候选（词频级质量）
- 密码框架构性隔离（TSF 框架层保证）

## 已知限制

- zh-CN profile 下字母进入拼音组合（默认；设置里"拼音组合模式"关闭后退回纯直通）
- 候选上限 60（每页 9，数字键 1-9；翻页：PageUp/PageDown、-/=、鼠标滚轮）；Viterbi 整句 + 变体质量闸；语义级消歧已上线为 LLM 重排（默认关，设置可开）
- 自绘光标应用（部分 Electron 编辑器）候选窗定位可能不准（已加系统插入符兑底）
- 设置界面已上线（托盘 + 即时生效 + 用户词典管理）；符号面板已上线（全局热键 Ctrl+Shift+B 调出，或单实例转发/启动参数）；emoji 经 GDI 灰度渲染（彩色需 DirectWrite）
- 中文续写走 ChainPredictor（纯 ASCII 上下文词表优先语言闸 + 复读过滤）；LLM 候选质量调优持续
- 32 位与 64 位宿主均支持（32 位 TIP 见 R5）；未签名（SmartScreen/杀软可能告警，无证书）
- e2e 在"用户正在用机器"时会受 ctfmon 前台竞态干扰（重试机制 + 就绪等待已缓解）
- 重建后已运行的应用继续用旧版 DLL，需重启应用或注销重登才会换新

## 工程资产

- `tools/fix_readings.py` / `tools/modern_terms.txt` / `tools/build_pinyin.py --modern-only`：词库数据管线（幂等、审计、闸门）
- `tools/pf_ux.cpp`：真实键速节奏性能探针（每键 RTT / 同 ver 双帧抖动 / UI 进程 CPU 采样）
- `tools/engine_test/b3_probe.cpp`：引擎行为探针（首字母/音节兜底直查候选）；`tools/pf_shot.cpp`：UI 走查截图工具（假客户端触发候选/翻页/toast/长文态）
- `tools/regress.cmd`：一键全量回归（engine + 词库 --check 闸 + deep + e2e；环境沙盒与自有进程重启）


- `tools/build.cmd`：锁文件预改名（被 CTF 加载的 DLL 能改名不能覆盖）
- `tools/register.cmd` / `unregister.cmd` / `imecheck.ps1`：注册与自检
- `tools/e2e/e2e.cpp`：端到端测试宿主（合成 ITextStoreACP + 模拟按键）
- `tools/build_dict.py`：词表加工（g10k + jieba → en/zh 词表）
- `deps/win-classic-samples`：官方 TSF 样本参照（step01-06 + tsfapp）

## 2026-10-30 增量（Loomy 自主推进轮）

- **模糊音组细分开关 ✅（`ef28cda`）**：fuzzyGroups 位掩码 5 组独立勾选（zh/z、ch/c、sh/s、n/l、前后鼻音），缺键=全组与历史一致；组变更热重建模糊索引；设置页 5 行子勾选；engine_test P8b 4 断言，regress 一次全绿（engine 218/0）。
- **R3 压测收口 ✅**：真机 63 样本 p50=62 / p90=140 / p95=187 / p99=453ms，打字 CPU 4%（单核），抖动 0——数据入 GAP §10。
- 已完成（同日）：候选窗透明度四档（`ef0e61d`）、模糊音 8 组（`7569b31`）。
- **选词联想 ✅（`04d589b`，GAP §11 主线一 P0 收官）**：上屏后语境联想+空格连续输出，
  deep_test T18a/b 真机 33/33（tmp/regress-2937629603）；assocAfterPick 默认开可关。
- **待办1 两项视觉验证 ✅（10-29 遗留结案）**：①设置页滚动条/底部裁切——GetScrollInfo 真值 + 真屏/PrintWindow 双截图证底部无裁切（黑条=DWM 边框伪影，PITFALLS #58）；②owner-draw 勾选（现 UI 实为 mkToggle 药丸）ON/OFF 两态多页实测渲染正确。顺带修 settingsui WM_VSCROLL 缺 SB_TOP/SB_BOTTOM case（程序化 Home/End 无效），构建+运行进程实测生效；收口后全量 regress 补跑一次 ALL PASS（regress-2538823020）。
- **记事本光标停滞根因修复 ✅（10-30 深夜）**：用户报障"上屏后光标停留原位"——TSF `InsertTextAtSelection` 选区零承诺 + 测试宿主全宽松掩盖 + Win11 记事本严格不动选区（PITFALLS #59）。修复=editsession.cpp 两 session 上屏后显式 SetSelection；deep TestStore 严格化建模记事本 + T19 光标契约用例（顺序+前缀+选区三元断言）；A/B in-vitro 复现（五红→35/35 全绿）+ regress 一次 ALL PASS（regress-494731470）。e2e 假红事件（负载噪声窗口+A/B/A 陷阱）入 PITFALLS #60。全项目深度分析落 docs/DEEP-ANALYSIS.md。
- **六项报障修复 ✅（10-30 夜二）**：#1 v 键（vMode 退役，v 走 PinyinStart 0 候选 echo）；#2 toast 吞候选（Show/SetPage KillTimer）；#3 补全窗漂移（GetTextExt 用插入点选区 pRange）；#4 右键菜单；#5 垃圾候选+翻页（Generate tier 闸 + 候选数设置化 + rerank 保页）；#6 符号面板三出口（热键切替/×/右键）。
- **引擎双根因收口 ✅（同轮）**：v-INITIAL 死头路径排除（vk→可 污染，PITFALLS #62）；longAllInit 近全跨度发射（HEAD 遗留 ≥4 声母纯简拼整条丢弃→gylgl 全灭，PITFALLS #61）——probe 实证 gylg→供应链|供应量|郭云龙|感应炉、gylgl→供应链管理、xyzxyz→0、垃圾墙不复活。engine_test 活基线阈值 40→20（PITFALLS #63：垃圾墙时代基线 vs 健康基线 23 + SKIP_BUILD 陈旧 exe 双重假红）。
- **验证状态（待收口）**：engine_test 重建单跑 0 FAIL、deep PASS、e2e attempt2 PASS（regress-2404128378）；**三套件全绿 regress + 真机六项复测留待用户停机后**——现役宿主 PID 24504 是 21:52 二进制（无 longAllInit）且带 test-NS split-brain（PITFALLS #64），须重启。15 文件 dirty 待回归绿后分批提交（HEAD=16e3e6c）。
- **候选窗工具条大版本 ✅（10-30 夜三）**：底部工具条（‹页码› 翻页按钮 + 固/符/设 按钮）+ 拖拽移动/位置固定（回显行/工具条空白拖拽，注册表 CandidatePin* 三键持久化）+ 按钮 tooltip/手型光标 + 右键菜单"复制该词/联网搜索该词" + 设右键换肤。构建全绿，probe 冒烟基线不回归；视觉验证待宿主重启。
- **交互二功率键批 ✅（10-30 夜三续）**：中键候选=置顶、Ctrl+左键=联网搜索、Shift+左键=复制、截断候选行悬停 tooltip 全词（换行重挂 TME_HOVER）、右键回显行=复制拼音、托盘"固定候选窗位置"勾选项（SetPinToggleCallback→CandWindow::TogglePin）。构建全绿，probe 基线不回归；交互验证随宿主重启一并做。
- **交互三托盘/滚轮批 ✅（10-30 夜三续二）**：托盘"开机自动启动"勾选项（HKCU\Run 单一真源，兼容早期 setup 已写键）、托盘 tip 状态化（暂停标注）、Ctrl+滚轮调候选字号（12–32 即时生效）、鼠标侧键翻页。构建全绿，probe 基线不回归。
- **交互四帮助+最近页 ✅（10-30 夜三续三）**：托盘"使用帮助..."速查窗（全交互集中可发现）；符号面板"最近"页（settings 新增 recentSymbols REG_SZ 字段，\x1F 分隔 ≤12 项，点击去重前移落盘，空态提示+智能默认标签）。engine_test 单跑 220 PASS/0 FAIL；probe 基线不回归。
- **交互五滚轮/快照/备份 ✅（10-30 夜三续四）**：符号面板滚轮换分类（11 页环绕）；帮助窗尾部"当前生效配置"实时快照段；托盘"导出/导入设置备份"（reg.exe export/import 整键，导入后热键重注册+tip 刷新，reg export 语义实证）。构建全绿，probe 基线不回归。
- **交互六皮肤/备份完整化 ✅（10-31 零点）**：皮肤 +2（茶棕/玫粉，5 处触点+钳制 4→6）；备份带日期文件名+用户词典随行导出/恢复（单文件完整迁移）；剪贴板占用 3×30ms 重试。engine_test 220 PASS/0 FAIL；probe 基线不回归。
- **交互七 11 项微功能 ✅（10-31 零点半）**：Shift+滚轮透明度；工具条空白右键换肤子菜单；符号面板 hover 高亮+手型+“日期”动态页（12 标签）；托盘暂停 30 分钟定时恢复/重载词典/打开数据文件夹/中键帮助/恢复默认（ResetSettingsToDefaults）；词典操作 1.5s 浮窗反馈（tooltip 窗+独立定时器）；--version 三级输出。构建全绿、engine_test 220/0、probe 基线不回归、--version 重定向实证。
- **交互八 20 项功能批 ✅（10-31 凌晨）**：候选行手型光标；右键"查该词拼音"；Ctrl+Shift+左键删除；中键修饰三合一（默认置顶/Shift 降频/Ctrl 删除）；符号面板 +4 分类（注音/希腊/框线/序号，14 标签）；右键"最近"标签清空；lastSymbolCat 分类记忆；托盘 tip 状态摘要（BuildTrayTip）；备份含热键短语随行（hotphraseui 新导出 HotPhraseDataPath/NotifyHotPhrasesChanged）；--help；--diagnostics（OS/DPI/词典/设置摘要）；日志轮转 >1MB→.old；lastSettingsPage 设置页记忆；Ctrl+Alt+P 全局热键切暂停；帮助窗 TOPMOST；设置窗居中打开；帮助窗 Ctrl+滚轮字号 12–28；EmitCliText 四级共享输出（WriteConsoleW/管道 UTF-16LE/CONOUT$/弹窗）。构建全绿、engine_test 220/0、probe 基线不回归、--help/--diagnostics 重定向实证、日志轮转实证（seed 1.2MB→.old 生成+新日志重开）。真机走查待 12:00 后与交互六/七一并做。
- **交互九 11 项功能批 ✅（10-31 凌晨续）**：托盘"暂停 1 小时"（2016）；Ctrl+Alt+H 全局热键开帮助；帮助窗"复制全部文本"按钮（剪贴板重试+按钮反馈复位）；帮助窗 Esc 关闭（正文 EDIT 子类化——焦点在 EDIT 时按键不冒泡）；设置窗 Esc 隐藏；界面字体族选择（uiFont 雅黑/宋体/等线/Segoe UI，RebuildFonts 按设置取 face、Show 即时生效、设置页下拉）；设置窗位置/尺寸记忆（setWin 四键、三隐藏路径落盘、恢复时显示器有效性校验）；词典窗搜索过滤框（词条/拼音子串实时过滤）；诊断摘要补字体字段；帮助快照补字体/明暗两行（cfg 缓冲 1600→4096 防 kHelpText 截断）；kHelpText 全量同步。构建全绿一次过、engine_test 220/0、probe 基线不回归、--diagnostics 实证含新字段。真机走查并入 12:00 后统一批次。
- **交互十 打字统计批 6 项 ✅（10-31 凌晨续二）**：打字统计核心（新 ui/usagestats.h/.cpp：今日/累计选词次数+上屏汉字数+活跃/连续天数；计数点=ApplyLearn 选词公共落点；每 20 次节流落盘+退出/WM_ENDSESSION 补写）；测试沙箱（LINGXI_SETTINGS_NS）自动停用防回归污染；打字统计窗（托盘 2017/设置·词典页入口；只读正文+刷新/清零（确认）/导出诊断报告按钮条；Esc 双路关窗）；导出诊断报告（诊断全文+统计 → lingxi-diag-时间戳.txt，UTF-8 BOM）；帮助快照/诊断摘要补打字行（BuildStatsBrief）；kHelpText 同步。构建全绿（修 .c_str() 警告）、**播种实证**（usage.txt 手写值经 --diagnostics 精确回读后清理）、probe 基线不回归、engine_test 220/0。真机走查并入 12:00 后统一批次。
- **12:00 收口计划**：见 tmp\plan_1200.md（重启宿主→regress 三套件→真机走查六~十→按 core/ui/docs 三批提交 24 dirty 文件，HEAD=16e3e6c）。
- **12:00 收口 ✅（10-31 午）**：全量 regress ALL PASS（regress-1439217053）；真机走查 smoke1 15/15（统计窗全流程/清零/导出/词典搜索/Ctrl+Alt+H/Esc）+ smoke2 8/8（设置窗矩形记忆移动→落盘→重启→精确恢复）；smoke 工装三坑入 PITFALLS #65（无 BOM ps1 中文字面串静默乱码）/#66（DLL 抢先重生宿主，带日志实例静默出局）——转发链代码零缺陷，前判"设置窗未创建"全系工装误判；测试残留全清（usage.txt/diag/SetWin*），用户词典 440 条完好；26 dirty 按 core→ui→docs 三批提交。
- **三批提交落地 ✅（10-31 午）**：d5380ba（core 10 文件）→ f6a98af（ui 14 文件，+2705 行）→ c1748d8（docs 3 文件），工作树全清。
- **架构/耦合评估报告 ✅（10-31 午）**：落 docs/ARCH-ASSESSMENT.md——实证构建图：三层（common←ime DLL←管道←ui 宿主）零层级违规、ImeCore 不依赖 ui（"层级倒置"不成立）；病灶在 ui 层内部：settingsui.cpp 2582 行 91 文件域符号 5 窗口类、window.cpp 1388 行、9+ 全局回调 setter。结论：**适合重构但为"文件拆分+接口收口"级（P0 立规→P1 拆 settingsui→P2 拆 window→P3 回调归并 UiHooks），不做架构重写**；安全网（220+deep+e2e+probe+smoke）已就绪，时机随功能节奏。