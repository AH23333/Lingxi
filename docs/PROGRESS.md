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
| M4.5 KV cache 增量 | ⬜ 未开始 | 退格回退、前缀复用（当前全量重评估已够快） |
| M5 拼音输入 | 🔶 默认已启用，真实应用激活已修复 | 组合窗口 + 拼音引擎（engine_test 9/9，延迟 0.32ms）+ 双 profile 双模式。真实应用拼音不激活根因已修（HKL 判据无效 → GetActiveLanguageProfile 真值 + 消息泵空闲点检测，见 PITFALLS #17/#18/#18b）；候选 5→9；光标矩形空值兑底。待用户实测确认 |
| M6 打磨 | 🔶 设置+符号面板已上线 | 托盘图标 + 设置窗口（拼音模式/英文补全/候选数量/明暗/中文标点/模糊音/Shift 切换/v 模式/双拼/繁体输出/热键开关 + 用户词典管理，HKCU 存储，即时生效）；符号面板（5 分类符号/emoji，点击 IPC 插入，emoji 代理对，托盘入口 + Ctrl+Shift+B 可开关热键）；单实例命令转发；**用户六项需求 A/D/B(含E)/C ✅（晚·十）**：自定义中英切换键五态（含 CapsLock）、快捷键句库（全局热键上屏，DLL 零改动）、自定义候选词（手动添加+拼音自动推断+提升/降低排序）、设置页流式重排+纵向滚动+可缩放美化。**皮肤 ✅（晚·十二 `bc7be9e`）**：全应用 WinUI 化——uistyle.h 共享设计令牌（中性灰阶+非蓝紫五皮肤：翠绿/青碧/橙/红/金）+DWM 圆角+设置页白卡 owner-draw+符号面板药丸浮层+词典窗/快捷短语窗统一（视觉待人工确认）。待做：卸载清理、代码签名 |
| 用户六项需求（A-F） | ✅ 6/6 完成 | A 切换键 ✅（`7a28923`）/ D 快捷短语 ✅（`7065b2a`）/ B+E 词典增强 ✅（`ebf0ae1`）/ C 设置页 ✅（`7bf02f5`）/ **F AI 预测 ✅ = R3 全量落地（核心前几轮分批 + 多路采样 `b3bad05`，见 R3 行）**；deep_test T16 加固 ✅（`c00e972` 免疫环境污染） |
| R1-R4 拼音体验 | ✅ 完成 | 词库 8 万→30 万 + 高频口语覆盖层；Viterbi 整句分词 + 末字变体；四层排序；模糊音；中文标点；候选翻页（PageUp/PageDown 与 -/=）；v 模式直通英文；Shift 单击中英切换；自学习用户词典（选字反馈 + 管理界面导入导出 + 新词入索引 + 简拼缩写调出）；小鹤双拼；繁体输出（OpenCC 映射）；符号面板；单实例命令转发；候选窗 hover 高亮；**B1 部分提交**（候选携带 cover=消耗的拼音前缀，选短词后剩余拼音留组合，学习键=消耗前缀根治错位学习）。engine_test 全 PASS |
| 深度体验整改（Loomy backlog） | ✅ B1/B8/B4/B3/B5/B2 全部完成 | B1 部分提交全链路 ✅（b7d0ee1+f84f049，cover 协议）；B8 符号热键默认关 ✅（06548a8）；B4 英文上下文词表优先+LLM 候选纯 ASCII 过滤+复读裁剪升级 ✅（2b3b477，ChainPredictor/StripContinuationEcho 移入 ngram.cpp 供单测，engine_test +14 断言，probe-chain 真机取证）；B3 零声母 i/u 键死 ✅（ZeroInitialShorthand 20 项表 DP 内扩展 + SyllUnit.rawLen cover 回放，engine_test P6d +8 断言，deep_test T15 10 Report 行为矩阵；同轮根治测试基建楔死：程序化激活换 ProfileMgr API，PITFALLS #32）；B5 Shift 单击切换断言 ✅（deep_test T16 四断言 + TapKey/RawKeyUp 辅助；顺带修组合中 VK_SHIFT 落 unmapped-default 误提交首选，分发层单列放行；flaky 归因方法论 PITFALLS #33）；B2 候选窗拼音回显 ✅（Show 加 composing 参数纯 UI 内部，DLL 零改动；PredictProc 传组合串，重排线程同 ver 穿透；高度 +1 行，hover/点击行判定偏移，GDI 实色近似半透明区分；窗体验证像素取证确认——探针 MSG_CTX 驱动真实窗出窗+特征色分析，pinyin 有回显带/en 对照无且候选区下推 42 行因果实锤）；B5 草案② toggleMode 设置升级 ✅（shiftToggle(bool)→toggleMode 0无/1 Shift/2 Shift或Ctrl；settings 双读兼容+双写旧版无感；DLL 切换键状态机扩展 Ctrl+交错保护；设置页三态下拉；regress ALL PASS 派生路径端到端实证）。**backlog 收官**（详见 cur/LOOMY-HANDOFF.md） |
| R3 LLM 整句重排 | ✅ 核心已落地（晚·十前分批）+ 多路采样收官（晚·十一 `b3bad05`） | 方案 cur/GLM_EXPLORATION_R3_LLM.md 分批落地：DLL 补发上文（`e328a7d` CTX_F_CTX_PREFIX+committed）→ 双上下文 KV 物理隔离/prefill abort 500ms 硬闸/EMA 吞吐自动降级/SEH 熔断（`4ca0218`）→ 重排默认关（`684d22a`）→ **多路采样 `b3bad05`**（贪心+采样双路：续写 fork 合并候选 seed=hash(prompt) 防闪烁；拼音重排 Borda 投票，top-1 不一致回退原序；llmMultiPath HKCU 默认关，设置页开关；采样链符号全可选缺则禁用）。旧文"未实施"系过时定性 |
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
- 候选上限 9 个（数字键 1-9，可在设置中调小；翻页已上线：PageUp/PageDown 与 -/=）；长拼音串分词已用词频 Viterbi 升级（末字变体候选），语义级消歧（LLM 重排）待接
- 自绘光标应用（部分 Electron 编辑器）候选窗定位可能不准（已加系统插入符兑底）
- 设置界面已上线（托盘 + 即时生效 + 用户词典管理）；符号面板已上线（全局热键 Ctrl+Shift+B 调出，或单实例转发/启动参数）；emoji 经 GDI 灰度渲染（彩色需 DirectWrite）
- 中文续写走 ChainPredictor（LLM 优先、词表兜底），LLM 候选质量调优待做
- 仅 x64 宿主；未签名（SmartScreen/杀软可能告警）
- e2e 在"用户正在用机器"时会受 ctfmon 前台竞态干扰（重试机制 + 就绪等待已缓解）
- 重建后已运行的应用继续用旧版 DLL，需重启应用或注销重登才会换新

## 工程资产

- `tools/build.cmd`：锁文件预改名（被 CTF 加载的 DLL 能改名不能覆盖）
- `tools/register.cmd` / `unregister.cmd` / `imecheck.ps1`：注册与自检
- `tools/e2e/e2e.cpp`：端到端测试宿主（合成 ITextStoreACP + 模拟按键）
- `tools/build_dict.py`：词表加工（g10k + jieba → en/zh 词表）
- `deps/win-classic-samples`：官方 TSF 样本参照（step01-06 + tsfapp）
