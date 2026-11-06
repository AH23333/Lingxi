# 灵犀输入法 — 开发日志（DEVLOG）

> 按时间顺序记录每个里程碑做了什么、为什么这么做。踩坑细节见 [PITFALLS.md](PITFALLS.md)，当前状态见 [PROGRESS.md](PROGRESS.md)。

## 2026-10-26 · M0 脚手架 + M1 TSF 骨架

**决策：两进程架构。** TSF DLL 被加载进所有宿主应用进程，DLL 崩溃 = 宿主崩溃，所以模型推理和候选窗 UI 必须隔离到独立进程（LingxiUI.exe），DLL 只做按键转发/上下文读取/上屏。微软拼音同架构。

- 生成 CLSID/双语言 profile GUID（en-US 主配置 + zh-CN 便于中文测试）
- 稀疏克隆微软官方 TSF 样本（`Windows-classic-samples/.../tsf/textservice/`，含 step01-06 分步教程）到 deps/ 作对照
- **注册攻坚**：先尝试 per-user（HKCU）——CLSID 可以写进去，但 CTF 的 `ITfInputProcessorProfiles::Register` 非管理员一律 E_FAIL，只能提权注册；第一次提权后 Register 仍失败，根因是 **InprocServer32 路径含空格必须加引号**（"D:\Visual Studio Code\..."）。修复后注册成功。
- 编译脚本被 GBK 坑了一次：cmd.exe 按 ANSI 读 .cmd，UTF-8 中文注释直接乱码，批处理一律纯 ASCII。

## 2026-10-26 · M2 候选窗 + 上屏

- LingxiUI.exe：置顶无边框候选窗，`WS_EX_NOACTIVATE` 保证点击不抢宿主焦点，深浅色自适应，DPI 感知
- 上屏实现两种：`ITfInsertAtSelection`（纯插入）+ 范围 `SetText`（英文前缀替换，协议加 `replaceLen` 字段——否则 "mor"+Tab 会变成 "mormorning"）
- 鼠标点选候选发生在 UI 进程，跨线程封送：pipe → DLL 的 IO 线程 → PostMessage 到 DLL 内隐藏窗口 → 宿主 UI 线程执行编辑会话

## 2026-10-27 · e2e 自动化测试（自我测试的转折点）

用户要求自己测。写了 TSF 标准做法的测试宿主：合成 `ITextStoreACP` 文本存储 + `AssociateFocus` 绑定真实窗口 + `ITfKeystrokeMgr` 模拟按键 + 直接断言缓冲区。

**自测揪出 4 个真实产品 bug**（详见 PITFALLS.md 前四条）：

1. `AdviseKeyEventSink` 的 `fForeground` 必须传 TRUE——否则按键永远不路由给我们
2. `MSG_ACTIVATE` 在管道连接前发送被丢弃 → UI 永远不知道谁是活动客户端 → 候选窗永不显示。修复：连接建立后补发快照
3. 管道写入没串行化：HELLO/ACTIVATE（管道线程）与 SendCtx（宿主 UI 线程）并发写帧交错损坏。修复：整帧写入持锁
4. **`ITfThreadMgrEventSink::OnSetFocus` 参数顺序搞反**（SDK 语义：第一参数=获得焦点的文档）。挂错方向后，每次焦点切换编辑 sink 就停在旧文档上，预测从此失灵——**这正是用户在真实应用里"切几次窗口就没有候选窗"的根源**

另确认两个环境事实：候选窗应跟随"最后打字的应用"（多客户端时 ACTIVATE 时序不如 CTX 时序可靠）；e2e 会与 ctfmon 抢前台（用户在用机器时测试有干扰，测前需要重试机制）。

## 2026-10-27 · M3 真词表引擎

用户反馈"只出英文且无候选窗"——分析为两个叠加原因：(1) v1 直通形态本来打不出汉字（拼音在 M5）；(2) 占位引擎只有 36 个词。

- 数据：google-10000-english（英文 1 万词，词频序）+ jieba 词库（MIT）取 top 80k 中文词（过滤含 ASCII/纯符号），`tools/build_dict.py` 加工
- 引擎：线性前缀匹配（1 万词 <0.1ms），英文保留用户大小写；中文输出高频接续词（诚实定位：无共现语料，质量占位，M4 LLM 接管）
- 健壮性：transitory 上下文（CUAS 宿主）只支持同步编辑会话 → 所有 RequestEditSession 改为"同步优先、TS_E_SYNCHRONOUS 降级异步"
- e2e 全绿（15 项断言 PASS，exit=0）

## 2026-10-27 · M4 本地 LLM 引擎（进行中）

用户要求"多用子代理全力开发"。三线并行：资源线（llama.cpp 预编译 + Qwen 0.6B GGUF，ModelScope 源）/ 引擎线（LoadLibrary 动态绑定，不参与链接，`__has_include` 探测头文件——deps 未就绪时自动编译为空实现）/ 测试线（engine_test 单元测试 + e2e 抗干扰重试）。

设计要点：
- 生成走最稳路径：`llama_decode` + `llama_get_logits_ith` 自实现贪心 argmax（采样器 API 跨版本变动大，logits 路径十年稳定）
- 结构体参数（model/context params）来自**与二进制同 tag 的头文件**，避免布局漂移；函数指针全部 GetProcAddress，任一缺失 → 整体降级
- 管线异步化：CTX 入队（每客户端只留最新）→ 预测工作线程（120ms 防抖 + 300ms 预算）→ LLM 空/超时 → n-gram 兜底 → alive 检查后应用

## 2026-10-27 深夜 · M4 集成完成（电池供电收尾）

三线并行结果：资源线（代理A）成功——llama.cpp **b11900** 运行时 + Qwen2.5-0.5B-Instruct Q4_K_M（ModelScope 直链 491MB，255 个 API 导出全验）；测试线（代理C）成功——engine_test 5/5 PASS（n-gram 预测 <0.3ms）+ e2e 重试壳（3 次尝试+前置净化+fail-fast）；引擎线（代理B 因并发配额失败，由主代理亲自实现）。

**集成期新坑两条**（详见 PITFALLS #16）：
1. 新版 llama.cpp 必须显式 `ggml_backend_load_all()`，且它**按当前工作目录**扫描后端 DLL——LazyInit 临时切 CWD 到 llama 目录再恢复
2. llama.cpp 版本号已改语义化（v0.7.0 是纯源码 release），预编译二进制挂在 bXXXX tag 下；ggml 头文件挪到了 `ggml/include/` 子目录

**LLM 实证**：`--probe-llm` 同步探针——模型 0.3s 加载，`今天天气不错，`→候选`今天`（703ms 首测，含 mmap 预热）。降级链改为**上下文感知**：词中→词表补全（replaceLen 语义），词边界/中文→LLM 续写。

e2e 最终回归 exit=0（20 PASS，重试壳实测生效）。收尾时设备转电池供电，预热性能收尾（Defender 首扫慢）移交下一阶段——完整交接见 [HANDOFF.md](HANDOFF.md)。

## 2026-10-27 深夜 · M5 启动崩溃排查与修复（三段式）

接手时的阻塞问题：M5 代码合入后 e2e 启动即崩 `0xC000041D`，门控（LINGXI_PINYIN=1）后仍崩、崩前无输出。

**第一步：取证。** e2e 里注册 first-chance VEH，把异常码/RIP/调用栈按"模块名+RVA"写入 `%TEMP%\lingxi-e2e-crash.txt`；用链接 map（ImeCore.vcxproj 加 `/MAP`）离线解析函数名。没有这一步，"崩在 ntdll 堆"完全无法下手。

**第一段：堆损坏根因。** 栈解码出 `ActivateEx→Deactivate→CPipeClient::Send→ntdll 堆 AV WRITE 0x24`：激活 sink 被 AdviseSink 到 ITfThreadMgr 的 ITfSource（错误对象，文档要求 ITfInputProcessorProfiles 的 ITfSource），写坏 CTF 内部 sink 表，之后任意堆分配即爆。崩溃点不等于肇事点。修复：移除错误注册（连带发现：即便挂对对象 CTF 仍 CONNECT_E_CANNOTCONNECT，最终彻底不用激活 sink）。

**第二段：键路径 TSF 查询副作用。** 修掉崩溃后拼音模式下候选窗全部失锚（SendCtx rect 全零）。定位：`GetActiveLanguageProfile` 在 CTF 按键分发内调用会触发输入上下文栈重算（多出 sandwich 文档、GetTextExt 失效）。模式检测改为纯只读信号：`GetForeground`（我们是否前台 TIP）+ `GetKeyboardLayout`（语言是否 zh-CN），按键路径零 CTF 重入。

**第三段：框架 churn 遏制。** 语言切换后打拼音，`StartComposition` 内 textinputframework 读 -1 指针 AV；后续又发现 CTF 弃用旧 store sink 不通知导致陈旧 vtable DEP（执行堆地址）。产品侧：`HandlePinyinKeySEH` 全程包裹 + `AbortCompositionState` 安全复位并降级直通；测试宿主侧：TestStore::RequestLock 同样 SEH 包裹。**TIP 铁律：任何框架异常不能带崩宿主。**

**附带修复**：build.cmd 的固定 `.prev` 改名策略在旧文件被进程映射（不可删）时失效 → LNK1168；改为随机后缀改名。e2e 增加 LingxiUI 就绪等待（轮询候选窗创建，替代盲睡 1.2s——冷启动+Defender 首扫可远超 1.2s，管道未连接时 CTX 会被设计性丢弃）。

**验证**：engine_test 9/9 PASS（含拼音 4 项，延迟 0.32ms）；e2e 默认门控 exit=0、LINGXI_PINYIN=1 也 exit=0（tests 1-5 全 PASS，拼音组合用例在合成宿主受语言 churn 限制为诊断级，进程零崩溃）。真实宿主拼音体验待 Win+Space 手工验证。

## 2026-10-27 深夜（二） · 真实应用反馈修复

用户实测反馈：所有应用里 zh-CN 都只有英文候选（打 ji 出 jimmy）、候选窗不跟随光标、候选太少、没有设置界面。

**拼音模式在真实应用全不激活的根因**：纯 TSF TIP 切换 profile 时线程 HKL 不变，之前的判据 `LOWORD(GetKeyboardLayout)==0x0804` 永远为假。e2e 的 `SwitchLanguage` 用 `LoadKeyboardLayout` 伪造了真 HKL，把问题完全掩盖——这是"测试全绿、真实应用全废"的教训。修复：判据改为 `GetActiveLanguageProfile(CLSID_LingxiTextService)` 的真实激活档案（zh-CN 档案 → 拼音模式），调用时机挪到封送窗口消息（消息泵空闲点，避开按键分发与激活回调两个雷区，修正 PITFALLS #18 的过宽结论）。

**其他修复**：候选窗光标矩形空值兜底（GetTextExt 成功但全零视同失败，逐级退到系统插入符）；候选上限 5→9（数字键 1-9 本来就支持，各引擎同步放宽）。

**用户诉求登记（M6 提前）**：设置界面（候选数量/深浅色/模式开关/热键）——提升为 M6 第一项。合成宿主内拼音组合完整性仍受限（PITFALLS #19），真实应用验证靠用户实测反馈迭代。

## 2026-10-27 深夜（三） · M6 设置上线

用户要求先做设置页面等内容。交付：

- **托盘图标**（app.ico 四尺寸，PIL 生成“灵”字蓝色圆角图标；.rc 资源）：左键开/关设置窗，右键菜单（拼音模式快捷开关、设置、退出）；注册 TaskbarCreated 消息，explorer 重启自动恢复
- **设置窗口**：拼音组合模式、英文逐键补全、候选数量（1-9）、候选窗明暗（跟随应用/浅色/深色）；控件即时保存
- **存储**：HKCU\Software\LingxiIme（common/settings.*，DLL/UI 共用）；每次读取实时查注册表（4 个 DWORD，µs 级），改动即时生效无重启
- **接线**：DLL 拼音开关在 DetectPinyinMode 读设置（替代环境变量）；UI 在预测管线尾部裁剪候选数、覆盖明暗、英文补全关闭时对纯 ASCII 上下文清空候选；DLL 按键路径额外探测设置开关变化（µs 级单值读）→ 触发延迟检测，托盘切换后在当前输入窗口即时生效
- **验证**：全量构建 + engine_test 9/9 + e2e exit=0；设置链路冒烟（CandidateCount=2 → 全链路候选数=2）

经验：单编 vcxproj 的 OutDir 是工程私有目录（tools\e2e\bin、ui\bin），与 sln 的根 bin\Debug 不同——回归前必须跑 tools\build.cmd 全量构建，否则跑的是旧 exe（本轮两次踩坑）。Python [IO.File] 相对路径不随 PowerShell 的 cd 变化，需绝对路径。

## 2026-10-28 凌晨 · R2-R4 体验面全落地 + QC 深度质检 + 双代理协作成型

- **QC 修复合入 main（39535f5/2490ed5/db50e36）**：`_userDict` 三线程数据竞争（引擎 `_udMtx` 粗锁 + 引擎/设置界面两侧原子落盘）、DllUnregisterServer 失败向上传播（杜绝幽灵 TIP）、文档对齐。验证门：构建 + engine_test 119 PASS + e2e exit=0
- **符号面板全链路**：5 分类符号/emoji（码点级代理对处理）、点击经 MSG_INSERT_TEXT→封送窗口插入、托盘入口、Ctrl+Shift+B 热键（SymbolHotkey 可开关，动态注册/注销）、单实例命令转发（命名事件+通知窗口，宿主自动拉起的无参数实例不再抢占）；WM_APP_LX_INSERT 包 `_fSelfInsert` 防上屏后误弹候选
- **ReadFrame 丢帧修复**：单次 ReadFile 读入多帧时余量字节被丢弃（连发帧静默丢失）→ 调用方持久 rxbuf + 消费后 erase；DLL/UI 两处调用方适配
- **小鹤双拼**：Predict 入口 `ShuangpinToQuanpin` 转换后复用全拼链路（jbtm→今天 用例）；**繁体输出**：OpenCC STCharacters 4012 字映射（码点级代理对处理）；**用户词升级**：新词入主索引 + 首字母简拼索引（mm→已学新词），Learn 后即时重建
- **QC 专项审查介入**：GLM_REVIEW 三个 P1（代理对/诊断文件/selfInsert 守卫）全部在提交前修复；设置窗口 g_updating 门控修复了"打开即写 0"的注册表污染（历史脏数据已清）
- **用户实测确认**：Edge 无法输入问题正式结案（cur/USER-CONFIRM-EDGE.md）
- **R3/R5 预探索交付**：LLM 重排/多路采样/KV 复用全方案（cur/GLM_EXPLORATION_R3_LLM.md，API 对 llama.h 核实）+ R2 缺口/R4 插入点/R5 工程化指南（cur/GLM_EXPLORATION_R2R5.md）——开发代理照做即可，无需再探索
- **协作机制**：双代理经 cur/ 沟通（QC 每 20 分钟自动巡视，活跃期 QC 不碰代码/git，停滞期接管修复）；本节文档由 QC 代更

## 待办（移交下一阶段）

- [x] ~~M4 收尾~~：LLM 已实证工作；候选质量调优并入 R3（方案已就绪）
- [x] ~~M4.5~~：KV 复用方案已入 R3 专文（§4），实施时按方案做
- [x] ~~M5 拼音输入~~：已上线（双拼/繁体/自学习齐备）
- [x] ~~M6 设置界面/隐身热键~~：已上线（托盘"暂停预测"即隐身等效，不再新做热键）
- [ ] R3：LLM 整句重排 + 多路采样 + KV cache（按 cur/GLM_EXPLORATION_R3_LLM.md 实施）
- [ ] R4 尾巴：自定义短语、字体字号皮肤（字号进行中）
- [ ] R5：32 位 TIP、卸载清理、崩溃保护、签名、regress.cmd（按 cur/GLM_EXPLORATION_R2R5.md 实施）

## 2026-10-28 下午（Loomy 轮：深度测试 + 体验整改 + 数据管线）〔10-29 上午追记：本块为当日下午快照，物理位置在上（追加于"晚·十四"块之后），时间线以标题日期为准〕

- **13 提交落地**（链见 git log）：R3 重排在制品全部修完（双上下文 KV 物理隔离 / prefill abort 硬闸 / EMA 吞吐降级 / SEH 熔断 / ClientCtx shared_ptr 悬垂修复 / rrSeq 连击合并），重排**默认关**——实测打字 CPU 66%→3.5%、候选窗抖动清零、续写往返 141→141ms 不被驱逐；
- 体验修复：提交键候选有界等待 ≤80ms + 拼音免防抖（快打空格不再上屏字母，每键候选 RTT 均值 57ms）；切分完整/残段分池（长串候选 27→60）；模式检测激活竞态定时器自愈（冷启动 deep_test attempt1 转绿）；
- 词库管线：多音读音系统补全 tools/fix_readings.py（228 字/239 读音 + --check 闸门进回归）；现代词层 tools/modern_terms.txt（176 词，inject_modern 只升不降、--modern-only 幂等）；engine_test 沙盒化 + 多音/现代词断言 119→142；deep_test 新增 T12/T13 行为场景；
- 收口前新报反馈全部取证完毕（本块快照时点未动刀；B1 部分提交/B6 隔音+eat/B7 滚轮随后由晚·十三/十四线程落地，勿重复施工），剩余项设计与复现步骤见 cur/LOOMY-HANDOFF.md §3 B3-B8 及其时间错位声明。

## 2026-10-28 晚 · B1 部分提交全链路（cover 协议）

**问题**：长串拼音选短词（如 nihao 选"你"）后剩余拼音被整串清空，"白打"。

**方案（LOOMY-HANDOFF §B1 草案落地）**：候选携带"消耗的原始拼音前缀字符数"（cover），提交后剩余拼音留组合继续。

- 引擎侧（b7d0ee1）：`Prediction.cover` 与 cands 平行；`Generate` 逐路径回放 DP 转移算 coverEnd，`Predict` 经 `inOfCtx` 映射回原始 context 字符数；双拼（键位映射不可逆推）/非 ASCII（UTF-8 变长错位）/用户词典 boost（保守）一律 0=旧行为
- 协议：MsgCands 帧在字符串列表之后**可选**追加 u16 cover×count（kProtoVer 不变）：旧 DLL 顺序读串即停容忍尾部；新 DLL 读不满按全 0——双向兼容
- UI 管道：重排按同一排列搬运 cover；**ApplyLearn 学习键改用消耗前缀**（raw.substr(0,cover)）——根治"nihao 选你却记到 nihao→你"的错位学习，学 ni→你 后打 ni 即命中且不影响 nihao 查询
- DLL：`GetCandidateEx` 单临界区取词与 cover；`PinyinCommitHanzi(hanzi,cover)` 部分消耗时无组合模式改内存串再预测、内联重启组合承载剩余（重启失败回退全清）；空格/Tab/数字/未映射键/鼠标选词全提交点接通；未映射键路径部分提交后不清候选（防与在途回包竞态）
- 测试：engine_test P6c 系列 +P6c4 pin 你@idx7；deep_test T14 端到端（nihao 按'8'选你→余 hao→空格→你好，无字母残留）

**取证教训**：候选序 probe 必须以 fresh 沙盒实测为准——n/l 模糊音默认开，立号/理好/利好（lihao）插在"你"前面，交接记录的"你@idx6"实为 idx7（'8'）。deep_test 日志走 ANSI 管道中文必乱码，断言取证用码点 hex 转储（T14 模式）。

**验证**：regress ALL PASS（engine_test 147 断言 / fix_readings / deep_test T14 / e2e）。

## 2026-10-28 下午（二） · B8 符号热键默认关

- Edge 插件快捷键被吞的取证结论（LOOMY-HANDOFF §B4+）：唯一全局占用 = main.cpp 的 `RegisterHotKey(Ctrl+Shift+B)` 默认开；DLL 无键盘钩子。
- 修复：`symbolHotkey` 默认 false（对齐 llmRerank 的"不默认强加全局占用"原则）；设置页文案注明系统级独占与 Edge 冲突、托盘入口不变。
- 用户 HKCU 旧默认残留（SymbolHotkey=1）已清 0 并重启真实环境 UI——热键即时释放，Edge 插件快捷键恢复。
- latency 红复盘子：同机同时刻 A/B（cover 前引擎 10.47ms vs cover 版 10.7ms）证明当日 >10ms 均为机器负载漂移，非代码回归；机器忙时（Qoder/PyCharm/Edge 同跑）median 卡阈值属已知噪声，安静时 6.5-7ms。

## 2026-10-28 晚（二） · B4 英文上下文词表优先 + 复读过滤升级

**根因 A（英文打字吐中文候选）**：纯英文上下文在词边界（如 "Today is a nice " 尾空格）走 LLM 优先，qwen 续写吐中文。**根因 B（复读）**：StripPromptEcho 只裁头部×尾部字面重叠——尾部带标点/输出整段复读 N 遍两形态漏网。

**方案（LOOMY-HANDOFF §B4 草案落地）**：

- **接缝决策**：`ChainPredictor` 与回显裁剪从 llm.cpp（engine_test 不编译）移入 ngram.cpp，导出 `CreateChainPredictor`/`StripContinuationEcho`（predictor.h 声明）——engine_test 用 FakePredictor 替身直接单测路由，llm.cpp 只留装配
- **根因 A**：链路由加纯 ASCII 分支——上下文整段 <0x80 时词表优先；词表无果其余引擎兜底但**候选过滤为纯 ASCII**（cover 平行同步剔，保持 size 约定）；全滤空则无候选（英文打字出中文比无候选更糟）。词中词表补全（replaceLen 语义）、中文上下文 LLM 优先均不变
- **根因 B**：`StripContinuationEcho` 升级——空白/标点归一化比较（回显与尾部"只差标点"形态命中）、重叠上限 40 抬到 tail 全长、整段复读循环剥（**仅上轮全匹配才续剥**，"很好很好"类叠词部分重叠一次即止）、确认回显后清前导标点；k≥2 单字留缝维持
- **probe 盲区补齐**：`--probe-llm` 直连 LlmPredictor 不经链路由，新增 `--probe-chain` 走真实装配链端到端取证

**验证**：engine_test +14 断言（echo 矩阵 8 例 + 链路由/过滤 6 例）全 PASS，共 174+ 功能断言；probe-chain 真机："Today is a nice "→纯 ASCII 英文续写（day. I hope you have a good one.）、"inte"→词表 0ms 短路、中文上下文不变；probe-llm 无复读、增量一致。regress 功能面全 PASS（deep_test/e2e/fix_readings），pinyin latency 10.4-11.9ms 卡 10ms 阈值为已知负载漂移——A/B：HEAD 干净构建 12.2ms 比 B4 版还高（LLC 缓存污染，Qoder/Edge 同跑），非代码回归。真实环境 UI 已重启吃新二进制（PID 7616）。

## 2026-10-28 晚（三） · B3 零声母 i/u 键死修复 + 测试基建楔死根治

**根因（修前实证）**：`--probe-pinyin` 对 i/u/in/iu/ui/un/uo/ian 全部 **0 候选**——词典/音节表没有以 i/u 打头的规范写法（普通话拼音规定零声母必须改写：i→yi、u→wu、in→yin、iu→you…），首键 i/u 切不出任何音节。v 打头是 vMode 直通英文入口（设计如此，HKCU VMode=1），非 bug。

**方案（LOOMY §B3 草案落地，超出草案最小集）**：DP 切分循环内查表扩展，而非字符串预改写——零声母位置判定依赖 DP 边界（iao 里 i 是改写头还是 ia+o 的组成部分），预改写有鸡生蛋问题。

- 引擎（ui/engine/pinyin.cpp）：file-scope `ZeroInitialShorthand()` **全 20 项**表（i→yi/ia→ya/iao→yao/ian→yan/iang→yang/ie→ye/in→yin/ing→ying/iong→yong/iu→you/u→wu/ua→wa/uai→wai/uan→wan/uang→wang/ue→yue/ueng→weng/ui→wei/un→wen/uo→wo）；`EnumeratePaths` DP 每位置尝试 L=1..4 简写扩展（`SyllUnit{FULL, 改写后音节, rawLen=L}`，cost=2），与全拼/简拼候选由 DP cost 竞争自然共存
- `SyllUnit` 加 `rawLen`：Generate cover 回放改 `pos += rawLen>0 ? rawLen : text.size()`——i→yi 若按改写后 text 长度回放 cover 会错报 2（原始只按 1 键）
- v 打头不映射，vMode 通路保持；DLL 侧（ime/）**零改动**（键路径既有代码全兼容）

**测试**：

- engine_test P6d 8 断言（i→一 cover1、u→五 cover1、in→音/因 cover2、iu→有 cover2、ui→为/位 cover2、un→问/文 cover2、uo→我 cover2、ian→眼/言 cover3）——修后首轮全绿
- deep_test T15 10 Report 行为矩阵：i/u/in/iu 首键空格收字（一/无/因/有）、i+逗号合并上屏（一+，）、退格缩减（iu→BACK→SPACE→一）+Esc 取消无残留、v 直通（v 吃/k 放行）、vMode Esc/退格两路退出后 i→一、iu→有
- after 取证：i→[一|以|已|意…]60 候选、ian→[言|眼|盐…]60、uo→[我…]36 等全链正常
- latency 红复盘子：A/B（stash pinyin.cpp）基线 10.978ms vs B3 版 13.236ms，均在已知机器漂移区间（B4 期同二进制 10.4→12.2 摆动），非代码回归

**测试基建楔死（独立于 B3 的真 bug，同轮根治）**：B3 收口跑 regress 时四套连环红——deep_test `pinyin mode settle` FAIL（probe_q_eaten=0）×3 attempt 连续 6 轮复现，T0-T15 全空，e2e pass-through 三连败；但 DLL 激活态完全正常（ActivateEx ✓ / InitSinks hr 全 S_OK / 拼音模式=1 / 管道 ✓），且同二进制 30 分钟前刚 22/22 全绿——典型间歇楔死。

排查路径（全部取证驱动）：

1. `InitSinks: tmgr/tfocus/key` 三值全 0x00000000=hr 全 S_OK → 排除 sink 注册失败
2. `拼音模式: 1` 全程未翻 0 → 排除模式误判；PinyinKeyEaten 对无修饰 'q' 必吃 → 锁定"键没到 DLL"
3. OnTestKeyDown/OnKeyDown 入口加 LX_LOG 取证（LINGXI_LOG 门控，真机零成本）→ 楔死日志零键事件 → **msctf 根本没派发**
4. 会话快照：前台线程 HKL=0x08040804（微软拼音）、WinUserLanguageList 里 zh 项只有微软拼音（灵犀不在会话级列表）
5. GetActiveProfile 楔死时返回**灵犀自己**（lingxi=1）→ "被其他 TIP 抢占"假设证伪
6. 决定性证据：某 attempt settle 15 次探测全红后 **1 秒起 T2-T15 全过**（21/1，唯一 FAIL 就是 settle）→ 激活后 msctf 按键派发存在**可变长就绪延迟**（约 1s～60s+，随负载漂移）

**修复**：deep_test/e2e 程序化激活从旧 `ITfInputProcessorProfiles::ActivateLanguageProfile` 换成 Win8+ `ITfInputProcessorProfileMgr::ActivateProfile`（走完整输入法切换路径，ctfmon 侧状态同步完整；旧 API 只做线程内 profile 激活，派发就绪无保证），回退旧 API 兜底；deep_test settle 探测窗口 3s→15s 覆盖残余就绪延迟；settle 失败诊断追加 GetActiveProfile 取证。

**验证**：deep_test 修复后 **6 连 22/22 全绿**；regress 一键回归 **ALL PASS**（engine_test 174+ 断言含 latency 绿 / fix_readings / deep_test T15 / e2e attempt1 直接过）。真实环境 UI 已重启（StartTime 晚于 DLL 构建时间戳，PITFALLS #27 复核过）。教训固化 PITFALLS #32。

## 2026-10-28 晚（四） · B5 Shift 单击切换断言 + 组合中 VK_SHIFT 误提交修复

**范围**：LOOMY §B5 草案①（T16 断言）。草案②（shiftToggle→toggleMode 设置页升级）与③（全局热键）本轮未动——②是设置页 UI 改动待用户拍板，③=R4 尾巴大项明确收尾轮不动。

**顺带真 bug 修复（composition.cpp）**：写 T16 前审读链路发现——组合中 `KeyDown(VK_SHIFT)` 落 `HandlePinyinKey` `default:` 的"unmapped 提交首选"路径（composition.cpp:864），**组合中 tap Shift 会把当前首选误上屏**（用户"按 Shift 想打大写"场景被打断）。修法：HandlePinyinKey 在 CtrlOrAltDown 闸后单列 `wParam == VK_SHIFT` 原样放行——Ctrl/Alt 本体在 KeyDown 时 keystate 已置位、被 CtrlOrAltDown 覆盖，Shift 无该覆盖需单列（weasel 参照：KeyEventSink.cpp:189 将 VK_SHIFT 注册为 preserved key 单独处理，不当普通键分发）。修复只动分发层，OnKeyUp 的单击翻转判定（_shiftDown/_shiftUsed/_shiftWasActive）不变。

**deep_test T16（4 Report，TapKey/RawKeyUp 两个新辅助）**：

- TapKey（TestKeyDown+KeyDown+KeyUp 完整单击）+ RawKeyUp（裸 KeyUp，配合 TypeKey 组"按住期间打字"序列）
- T16a 空组合单击 Shift→切临时英文（'a' TestKeyDown/KeyDown 均 eaten=FALSE 直通）
- T16b 再单击切回中文（'a' 进组合 eaten=TRUE，Esc 可清）
- T16c Shift 按住期间打字→KeyUp 不翻转（模拟限制：GetKeyState(VK_SHIFT) 不随 pKm 模拟变，断言对象是 DLL 内部 _shiftUsed 判定非真实 Shift+字母直通）
- T16d 组合中单击 Shift：组合不丢（'i' 续组合）+ **首选不被误提交**（t2 空=VK_SHIFT 修复回归断言）

**验证**：deep_test attempt1 全绿 PASS=26 FAIL=0（22+4）；20 轮循环里 T16 自身零失败，LX_LOG 取证 `Shift toggle -> tempEnglish=1/0` 三次翻转对。regress：fix_readings/deep_test/e2e PASS，engine_test 唯一红=latency（A/B：同二进制 run1 6.9ms PASS / run2 13.1ms FAIL=已知本机漂移非回归）。

**flaky 归因记录（PITFALLS #33）**：T16 加入后 20 轮里 attempt1 偶发挂 1~7 个，取证确认全部为既有漂移非新回归——失败首发位置在 T14/T15（新增测试之前），且全部失败与单因一致（T14 "你hao"=部分提交后新候选 vs 空格 WaitForLatestCandidates(80ms) 竞态→CommitRaw 回退；attempt1 挂 7=T15 中段 _pinyinMode 漂移后下游全直通，T16b/c/d 失败均其症状）。attempt 间自愈（3 attempt 设计），全轮最终绿。

真实 UI 已重启（PID 18700，晚于 DLL 时间戳）。

## 2026-10-28 晚（五） · B2 候选窗拼音回显（backlog 收官）

**范围**：LOOMY §B2 草案全落地——非 inline 模式下用户看不到自己输入的拼音，候选窗首行回显组合串（weasel 参照：WeaselUI/HorizontalLayout.cpp:18 非 inline preedit 首行模式）。

**实现（纯 UI 内部，不走协议，DLL 零改动）**：

- `CandWindow::Show` 签名加 `const std::wstring& composing`（window.h/window.cpp）；`Hide` 清 `_composing`；新成员 `_composing`（临界区保护，与 _cands 同生命周期）
- `PipeServer::PredictProc`：`_wnd->Show(..., e.pinyin ? e.text : std::wstring())`——拼音请求带组合串，英文补全/续写传空（无组合）
- `StartAsyncRerank` 签名穿透 composing（按值捕获进重排线程 lambda，同 ver 不跳变，重排更新候选窗时回显不闪）；该分支 `e.pinyin` 必真，直接传 `e.text`
- `LayoutAndPosition`：composing 非空时高度 `+ itemH`；回显行文本（组合串+全角冒号）参与宽度竞争（长拼音撑宽窗口）
- `OnPaint`：首行绘制 `composing + "："`——底色与候选区微区分（GDI 无 alpha，dark RGB(53,53,53)/light RGB(240,244,250) 一档实色近似半透明），文字色 dark 用浅蓝 RGB(150,190,255)/light 用 accent
- **行判定偏移**（草案风险项：窗口高度变化影响交互定位）：`WM_MOUSEMOVE` hover 与 `WM_LBUTTONUP` 点击的行号计算减去 `pad + compH` 偏移——点回显行不选中任何候选（row 负数落 valid=false）

**验证**：构建全过；regress deep_test/e2e/fix_readings PASS，engine_test 唯一红=latency（13.3ms 已知本机漂移，B2 纯 UI 改动、engine_test 不加载 UI 代码，逻辑排除）。窗体绘制无自动化断言（草案风险注记），靠真机人眼验收——真实 UI 已重启（PID 29372）。

**backlog 状态**：B1/B8/B4/B3/B5/B2 全部完成，Loomy 体验整改收官。

## 2026-10-28 晚（六） · B5 草案② toggleMode 设置升级（收官后补遗）

**范围**：LOOMY §B5 草案②——`shiftToggle(bool)` 升级 `toggleMode(0 无/1 Shift/2 Shift 或 Ctrl)`，用户可自定义中英切换键。

**实现（三级生效模型内：设置+键盘侧同轮）**：

- `settings.h`：删 `bool shiftToggle`，加 `uint32_t toggleMode = 1`（注释含注册表双键说明）
- `settings.cpp` 双读兼容：新键 `ToggleMode` 优先（≤2 有效）；无/越界由旧键 `ShiftToggle`（bool）派生 0/1；两键均缺=默认 1（Shift，历史行为）。保存双写：`ShiftToggle = (toggleMode != 0)` + `ToggleMode`——旧版 UI/DLL 读旧键仍得正确开关语义（"Shift 或 Ctrl"细模式对旧版退化为"开"）
- DLL `service.cpp` OnKeyDown/OnKeyUp：切换键状态机从 VK_SHIFT 扩展到 VK_SHIFT+VK_CONTROL；KeyUp 按模式判键——Shift 在 mode≥1 允许、Ctrl 仅 mode≥2；**交错保护**：Shift/Ctrl 先后按下（Ctrl+Shift+K 类应用快捷键）视为组合使用标 used 不切换（原 `wParam==VK_SHIFT` 单键判定顺带覆盖此场景，扩展到 Ctrl 后必须显式处理）。service.h 注释更新（成员名 `_shiftDown/_shiftUsed/_shiftWasActive` 保留，语义扩为"切换键"）
- 设置页 `settingsui.cpp`：复选框（IDC_CHK_SHIFTTOGGLE=109 弃用）→ 三态下拉 IDC_CMB_TOGGLEMODE=117（关闭/Shift/Shift 或 Ctrl），CBN_SELCHANGE 即时保存；窗口高 704→742 补偿标签+下拉两行

**用户零感知**：HKCU 现值 ShiftToggle=1 无 ToggleMode 键 → 派生 toggleMode=1 = 原 Shift 单击行为不变。

**验证**：regress **ALL PASS**（engine_test 含 latency 绿 / fix_readings / deep_test T16 四断言在派生路径下全过=双读兼容端到端实证 / e2e attempt1）。mode 0/2 的 HKCU 两态路径不测（污染风险，靠代码审读，同 T16 既有口径）。真实 UI 已重启（PID 26956）。

## 2026-10-28 晚（七） · B2 候选窗回显像素取证（验证补遗，收官后收口）

**动机**：B2 落地时窗体绘制无自动化断言（"人眼验收"），本轮用探针+像素分析把回显行的真实渲染补成实证。

**方法**：假客户端探针（fakeclient 改版）——连 `\\.\pipe\LingxiIme` 发 HELLO/ACTIVATE + MSG_CTX（pinyin=1, text="nihao", caret=(200,400)）驱动真实 LingxiUI 候选窗出窗；PowerShell System.Drawing CopyFromScreen 截 caret 下方区域；特征色像素分析（设计里独有的 RGB 值计数+行分布，无需视觉模型）。

**结果（两态对照，因果锁死）**：

- **pinyin 模式**：composing 底色带 RGB(240,244,250) 位于窗顶 42 行横带（6609 像素）→ 其下窗体底色 RGB(250,250,250) 候选区 → accent RGB(0,120,215) 跨回显带（composing 文字用 accent 蓝）+ 延伸首候选行底。回显行绘制/位置/配色三项全符合设计。
- **en 对照**（flags=0, text="inte"）：compBg=0（无回显带），候选区整体上移 42 行（=回显带高度）——**回显行只在有组合串时出现且候选区同步下推**。

**边界**：探针短暂抢占管道 active，断开后归还真实客户端；截图坐标系受 DPI 虚拟化影响但特征色空间关系不受影响（只看相对行分布）。探针/截图临时产物已清理（tmp 下零残留），像素数据固化于 cur/LOOMY-HANDOFF.md §B2 补遗。

**backlog 终态**：B1/B8/B4/B3/B5(①②)/B2 全部完成且 B2 窗体验证补齐实证，Loomy 体验整改全链收官。

## 2026-10-28 晚（八） · R5 稳定性收口：WER 崩溃转储落地 + SignalNamedEvent 静默失败 P2 修复

**背景**：cur/GLM_EXPLORATION_R2R5.md R5"稳定性"节——WER LocalDumps 脚本（tools\setup_dumps.cmd）早已就位但**从未在本机执行**（HKLM 键缺失实证）；同节"顺手"条 SignalNamedEvent 4 秒静默失败为遗留 P2。

**改动**：

- `ui/main.cpp` `SignalNamedEvent`：4 秒等待耗尽（主实例持有单实例 Mutex 但通知事件未建——冷启动 LLM 加载可达数秒的真实窗口）不再静默丢命令，`MessageBoxW` 弹一次"主程序仍在启动中"提示（`static bool warned` 保证进程存活期至多一次，--symbols+--settings 双超时也只弹一盒），前置 LX_LOG 取证。测试无依赖（全仓 grep 无 --settings/--symbols 外部调用方），纯二实例回退路径。
- **WER LocalDumps 本机执行**：`Start-Process -Verb RunAs` 提权跑 tools\setup_dumps.cmd（UAC 一次），注册表实证 `HKLM\...\LocalDumps\LingxiUI.exe` DumpFolder=`%TEMP%\lingxi-dumps` / DumpType=2（全量）/ DumpCount=5——此后任何 LingxiUI.exe 崩溃自动落全量 dump 离线可析。DLL 侧不装 VEH/UEF 铁律不变（宿主进程内装全局异常处理器=事故源），零代码方案优先于 SetUnhandledExceptionFilter 代码方案。

**验证**：build Debug 全过；regress **ALL PASS**（engine_test / fix_readings --check / deep_test / e2e attempt1）；真实 UI 已用真实 APPDATA 重启（PID 20772， StartTime 晚于构建）。本工具 shell 的 APPDATA 实测即用户真实 Roaming 路径（嵌套 powershell 覆盖语句被外层插值吃掉的坑固化 PITFALLS #34）。

**R5 剩余**：32 位 TIP（vcxproj 加 Win32 配置 + EnsureUIRunning 回退链，unregister.cmd 已前向兼容 Debug32 路径）；R3 LLM 整句重排仍为方案就绪未实施（前置堵点：拼音模式 DLL 补发上文）。

## 2026-10-28 晚（九） · R5 32 位 TIP（WOW64）全链落地

**范围**：cur/GLM_EXPLORATION_R2R5.md R5"32 位 TIP"节——半天预估项，本轮全链完成（构建/注册/回退链/验证工具/真机三环取证）。

**实现**：

- `ime/ImeCore.vcxproj` + `common/common.vcxproj`：加 `Debug|Win32` + `Release|Win32` 配置；Win32 输出分离到 `bin\<C>32\`（用 `MSBuildThisFileDirectory` 免 SolutionDir 依赖——直编 vcxproj 时该属性为空，路径会塌到项目目录）；x64 产物路径与构建零变化（条件属性组只对 Win32 生效）。
- `ime/pipeclient.cpp` `EnsureUIRunning` 回退链（探索文点名的"唯一代码坑"）：32 位 DLL 目录（`bin\Debug32\`）没有 x64 `LingxiUI.exe`——同目录尝试失败后按 DLL 自身配置名（路径含 `Debug32`→`Debug`）回退 `..\Debug\LingxiUI.exe`（32 位进程拉起 x64 exe 合法），仍失败降级纯直通（既有安全路径）。
- `tools/build32.cmd`：仅编 ImeCore+common（vcvars64 下 msbuild `/p:Platform=Win32` 自动选 Hostx64\x86 交叉工具链），LingxiUI/engine/e2e 保持 x64。
- `tools/register.cmd` 扩展：64 位注册后若 `bin\<C>32\ImeCore.dll` 存在则 `SysWOW64\regsvr32` 注册 32 位（WOW6432Node 视图，与 x64 注册共存）；`tools/unregister.cmd` 早已前向兼容 Debug32 路径。
- `tools/host32.cpp` 探针留存：32 位安装验证宿主——deep_test 同款文本宿主骨架（窗口+ThreadMgr+TestStore+AssociateFocus，**TIP 实例化必须有焦点上下文**，裸 ActivateProfile 不加载 DLL）+ ProfileMgr 激活 + 模块枚举断言 `loaded ImeCore ... dir32=1`。x86 交叉编译（`vcvarsamd64_x86` + `/std:c++17`，log.h 内联变量需 C++17）。

**真机三环取证**（LINGXI_LOG=1 增量归因，日志同文件 [host32] 标记行+[pipe] 行）：

1. **加载环**：host32（PE 014C 真 32 位）内 `loaded ImeCore: ...\bin\Debug32\ImeCore.dll (dir32=1)`——32 位 msctf 经 WOW6432Node CLSID 正确加载 32 位 DLL。
2. **管道环**：`[pipe] 管道已连接` + ActivateEx/TextEditSink/拼音模式全流程在 32 位进程内跑通，连上 x64 UI。
3. **回退环**：杀 UI 后 host32 重跑 → `[pipe] 已拉起 LingxiUI.exe（32 位回退路径）` + UI 自动回归——EnsureUIRunning 回退链真机实证。

注册实证：`HKLM\SOFTWARE\WOW6432Node\Microsoft\CTF\TIP\{CLSID}` Enable=1 + `WOW6432Node\Classes\CLSID\...\InprocServer32` → Debug32 DLL；64 位注册不动。

**弯路（记入取舍）**：Win11 24H2 的 `SysWOW64\notepad.exe` 是商店应用跳板存根（3 秒即退，真身 64 位商店 Notepad）——探索文建议的"SysWOW64 notepad 打字看日志"验证不成立（首测日志实为 64 位 DLL 发的，SendKeys 还可能打到别处）；自建 host32 才是诚实路径。

**验证**：build32 + build（x64）双过；PE 位数核验（Debug32=014C / Debug=8664）；regress **ALL PASS**（engine_test/fix_readings/deep_test/e2e attempt1）；真实 UI 已重启（PID 9628）。

**R5 终态**：稳定性（WER+P2）✅ + 32 位 TIP ✅ + 卸载增强 ✅ + regress 常备 ✅——**R5 工程化整节完成**。剩余大项仅 R3 LLM（方案就绪未实施）。

## 2026-10-28 晚（十） · 用户六项需求：A 切换键/D 快捷短语/B 词典/C 设置页 四单元落地（F AI 预测留下轮）

**范围**：用户六项需求（A 自定义中英切换键 / B 自定义候选词 / C 设置页可缩放美化 / D 快捷键句库 / E 词典管理增强并入 B / F AI 预测=R3）——本轮完成 A/D/B(含E)/C 四单元 + deep_test T16 加固，F=R3 留下轮（核心输入路径大改，收尾段不塞）。

**A 自定义中英切换键（`7a28923`）**：

- `common/settings.cpp`：toggleMode 扩枚举 0 关/1 Shift/2 Shift或Ctrl/**3 CapsLock**/**4 Ctrl**（上限校验 2→4）；HKCU 双键兼容不变（ToggleMode 优先，旧 ShiftToggle 同步写）。
- `ime/service.cpp/h`：CapsLock 三态状态机——`_capsDown/_capsUsed/_capsWasActive`；`OnTestKeyDown`/`OnKeyDown` mode 3 时吞 CapsLock（**不翻转大小写锁定**），KeyUp 干净单击（未组合使用+按下时无组合+当前无组合+zh 模式）切中英+toast；Shift/Ctrl allowed 判定改精确枚举 `(tm==1||tm==2)`/`(tm==2||tm==4)`，杜绝 mode 3/4 时误判组合使用。
- 设置页五态下拉；deep_test T16 前置 pin（见文末 T16 加固）。

**D 快捷键句库（`7065b2a`）**：

- 新模块 `ui/hotphraseui.{h,cpp}`：HotPhrase{mods,vk,phrase}；存储 `%APPDATA%\LingxiIme\hotphrases.txt`（UTF-8 BOM 原子写，行格式 `mods,vk<TAB>句子`，上限 24 条，热键强制含 Ctrl/Alt 防占用打字键）；管理窗（热键录制框子类化 EDIT 捕获 KEYDOWN/SYSKEYDOWN、ESC 清空、重复/超限即时反馈）。
- `ui/main.cpp`：RegisterHotKey id=100 起 + WM_HOTKEY 分发，命中条目经 `g_insertText`→SendInsertText 上屏——**复用符号面板 MSG_INSERT_TEXT 链路，DLL 零改动**；保存后 SetHotPhrasesChangedCallback 全量重注册。

**B/E 自定义候选词与词典管理增强（`ebf0ae1`）**：

- 用户词典管理窗加手动添加词条（词条+拼音框，拼音留空自动推断）：`GetPinyinInfer` 懒加载 `zh_pinyin.txt`（300320 行）——整词精确命中优先、逐字单字条目首见读音连写回退、键口径小写无空格连写（=引擎 NormKey，带空格永不匹配）。
- 新词条 count=1000 进引擎置顶区；提升/降低选中（±1000，下限 1）——自定义顺序=引擎既有"用户词条置顶区按 count 降序"口径，**零引擎改动**；窗口 520x480 重排（EM_SETCUEBANNER 提示、7 按钮行）。

**C 设置页可缩放美化（`7bf02f5`）**：

- 流式重排布局引擎：`CtlItem{h,h2,pw,ph,lw,flags}` 注册表 + `LayoutSettings()` 按客户区宽度流式排布——加宽自动折多列、拖拽缩放有实际意义；IT_SECTION 段标题独行占满整行（段前双倍间距）、IT_WRAP 放不下换行、IT_BOTTOM 锚底（不早于内容尾，内容短时贴客户区底）。
- 纵向滚动：`UpdateSettingsScrollbar` 按内容高/客户区高差值动态挂/摘 WS_VSCROLL（FRAMECHANGED 会再触发一次 WM_SIZE，样式已一致收敛）；WM_VSCROLL（行/页/拖拽）+WM_MOUSEWHEEL（120→3 行）；坐标系统一虚拟坐标，MoveWindow 时才减 `g_scrollY`。**解死结**：无滚动时内容高度随宽度变化（min 尺寸依赖宽度，永远定不准），滚动把高度下限解放到 380。
- 窗口 WS_THICKFRAME|WS_MAXIMIZEBOX 创建（680x560），WM_SIZE/WM_GETMINMAXINFO（min 460x380 DPI 缩放/max 1600x2200）/WM_CTLCOLORSTATIC（浅底 RGB(248,250,252)+灰黑文字）；控件注册式创建 addSection/addCheck（MeasureTextW 实测宽）/addComboPair（标签实测+SS_CENTERIMAGE 垂直居中）/addButton。

**deep_test T16 加固（`c00e972`）**：环境 ToggleMode=4（设置页烟测点组合框残留）致 T16 三连挂——DLL 每事件实时读 HKCU，测试隐式依赖环境。T16 块开头 `PinToggleMode(1,&old)` 固定 Shift 语义+结束 `RestoreToggleMode(old)` 还原（旧键 ShiftToggle 同步，崩溃残留=默认值无污染）。

**验证**：D 轮 regress ALL PASS；C 轮 P/Invoke 探针烟测——默认尺寸滚动条自动出现+SB_LINEDOWN×3 位移精确 Δ=90（3行×30px）、拉大至 1000x1000 滚动条自动消失（style 0x14CF0000 无 0x200000）+复选框双列折行（x=16/x=466 同行）+提示贴底（915+30+16=961=客户底）、段标题全程占满独行、用户词典/快捷短语子窗可达；T16 加固后 regress **ALL PASS**（engine_test/fix_readings/deep_test/e2e attempt1）；真实 UI 已重启（PID 25320，晚于构建）。

**坑固化**：PITFALLS #36（PS Set-Content 编码损坏中文源码——批量替换必须 edit 工具）/ #37（FindWindowW $null 封送空串）/ #38（冷加载期间触发二实例超时挂错误框）/ #39（探针 -band 插值字面输出假绿）/ #40（环境依赖测试必须 pin + edit 多匹配落错块）。

## 2026-10-28 晚（十一） · F AI 预测=R3 多路采样落地（`b3bad05`）

**范围**：F 项（AI 预测）= R3 方案（cur/GLM_EXPLORATION_R3_LLM.md）最后一块代码——§3 多路采样。**先纠正定性**：DEVLOG/PROGRESS 旧文"R3 未实施"已过时——R3 核心（DLL 补发上文 e328a7d / 双上下文 KV 物理隔离+prefill 硬闸+EMA 降级 4ca0218 / 重排默认关 684d22a / 拼音两阶段重排）前几轮已全部落地；本轮补齐方案唯一剩余代码项。

**多路采样（`b3bad05`）**：

- `common/settings.{h,cpp}`：新字段 `llmMultiPath`（HKCU `LlmMultiPath`，默认关——每次 LLM 调用约 +150ms CPU，实验特性手动开）。
- `ui/settingsui.cpp`：智能特性区新增勾选框（IDC_CHK_LLMMULTIPATH=119，"贪心+采样两路投票，重排更稳，再增 CPU"）。
- `ui/engine/llm.cpp`：
  - LlamaApi 绑定 10 个 `llama_sampler_*` 符号（chain_default_params/init/add/n、init_top_k/top_p/temp/dist、sample、free）——**全部 required=false**，缺任一 → `_samplerOk=false` 采样路整体禁用，贪心路零影响；LazyInit 自检四段链 chain_n==4。
  - 续写 Generate：贪心跑完后，门（llmMultiPath && 采样链可用 && KV 高命中 common≥90%·n && 贪心耗时 ≤60ms 慢机让位）通过则 fork 采样路——`seq_rm(0, n-1, -1)` 回滚到 prompt 末位前一格 + 重 decode 末位取新鲜 logits（**末位重评是 fork 正确性的关键**：贪心退出时 logits 对应生成中段，直接采样会采到错误位置的分布）；采样输出经 StripContinuationEcho/ SplitContinuation 后去重合并进候选（cap 9）。seed=FNV-1a(prompt token 序列)，同输入同输出防候选闪烁；与贪心共享 kBudgetMs 总预算。
  - 拼音重排 DoRerankInner：贪心解析抽 `ParseOrder`（dup/空 false、漏编号补原序）复用；门（增量 prefill ≤64 tok && 距 deadline ≥150ms）通过则 fork 采样路重新生成数字序，`BordaMerge` 两路全排列计分合并（score=Σ(K-位置)，并列取贪心相对序——res 为基底+stable_sort）；**两路 top-1 不一致保守回退拼音原序**（返回空）。采样失败/部分输出 → 只用贪心路结果。
  - MakeSampler 四段链 top_k(40)/top_p(0.9,1)/temp(0.7)/dist(seed)；逐段判空再入链（chain_add 对 null 无防护）；每 token 查 deadline。

**验证（本机当晚 left4dead2 后台 + 残留 UI 预热强噪声环境下）**：engine_test/fix_readings 全绿；deep_test 26/26（AV=0）；e2e 全绿（含 `mor` 直通/Tab 补全/international 真词典/中文续写链）；`jintianqi` 时延空载窗口 7.07ms（预算 10ms 内）。**归因方法论**：时序项红先查 `Get-Process | Sort CPU` 后台消费者 + taskkill 残留 LingxiUI，仍红再 stash 旧码重建 A/B——旧码同漂 7.2~14.2ms 同分布=环境非代码（详见 PITFALLS #41/#42）。

**坑固化**：PITFALLS #41（后台负载使时序预算漂移——stash A/B 定责方法）/ #42（残留 LingxiUI 跨 taskkill 存活污染 e2e——测前必杀）。

## 2026-10-28 晚（十二） · 全应用 WinUI 风格重做（去蓝紫，`bc7be9e`）

**背景**：用户最高优先约束——禁止蓝紫色调，尽可能符合 WinUI 风格，覆盖所有 UI（候选窗/设置页/符号面板/词典窗/快捷短语窗）。

**共享设计令牌（新文件 `ui/uistyle.h`）**：

- `SkinDef{accent,hoverLight}`；`inline constexpr kSkins[]` 五皮肤全非蓝紫：**0 翠绿 RGB(16,124,16)（默认）/1 青碧 RGB(3,131,135)/2 橙 RGB(202,80,16)/3 红 RGB(209,52,56)/4 金 RGB(152,111,11)**；`UiSkin(idx)` 越界回退 0。inline constexpr C++17 ODR 安全；候选窗/设置页/符号面板统一从 Settings.skinIndex 取色→全应用统一换肤。
- 中性灰阶常量：kUiBg(243,243,243)/kUiCard 白/kUiCardBorder(229)/kUiFlyoutBg(253)/kUiFlyoutBorder(224)/kUiText(26)/kUiText2(93)/kUiControlEdge(130)。
- `UiApplyRoundCorners(hwnd,w,h)`：优先 Win11 DwmSetWindowAttribute(33=DWMWA_WINDOW_CORNER_PREFERENCE, 2=DWMWCP_ROUND) 一次登记（静态 s_dwm[8] 槽+IsWindow 回收），系统抗锯齿且尺寸自动跟随；失败回退 CreateRoundRectRgn(0,0,w+1,h+1,8,8)，调用方每次改尺寸重调。

**候选窗（window.cpp/h）**：删本地 kSkins 改用 uistyle.h；wc.style 加 CS_DROPSHADOW；Init 后 UiApplyRoundCorners(240,120)，LayoutAndPosition/ShowModeToast 尺寸后重设；OnPaint 配色 WinUI 化（dark bg 44/border 66/fg 228，light=flyout 令牌）；行高亮改 **RoundRect 4px 圆角色块内缩 1px**（选中=accent 白字、hover=hoverLight，NULL_PEN 绘制）；组合回显去蓝（dark 文本 RGB(180,180,180)/light accent，bg dark RGB(54)/light kUiBg）；toast 中英分支同令牌。

**设置页（settingsui.cpp）**：

- 布局重构：SECTION 项 y+=rowH+MulDiv(28)，逐项记 it.fy；**卡片开合**（SECTION 开新卡/闭旧卡，几何={pad-8, fy-8, cw-pad+8, bottom+8}，flowEnd=contentEnd+16）；WM_ERASEBKGND 中性底+白卡 RoundRect 16 半径描边 kUiCardBorder 返回 1。
- **owner-draw**：addCheck 加 BS_OWNERDRAW→DrawOwnerCheck（20px 4 圆角方框：选中=accent+白圆头对勾 ExtCreatePen PS_GEOMETRIC|PS_ENDCAP_ROUND，未选中=kUiControlEdge 1px 描边；先 FillRect kUiCard 盖 BUTTON 默认 BTNFACE 擦除，BM_GETCHECK 取态）；addButton 加 BS_OWNERDRAW+g_accentBtns→DrawAccentBtn（accent 圆角底白字，ODS_SELECTED 加深 25%）；WM_DRAWITEM 按 ODT_BUTTON 分发。
- WM_CTLCOLORSTATIC（hint→kUiBg+kUiText2，其余→kUiCard+kUiText）/WM_CTLCOLORBTN/WM_CTLCOLOREDIT/WM_CTLCOLORLISTBOX 全接线；g_sectionHwnds 统一 FW_SEMIBOLD 13pt 雅黑段标题；WM_VSCROLL/WM_MOUSEWHEEL/UpdateSettingsScrollbar 重置路径全量 InvalidateRect 防残影（卡片+控件整体位移，局部重绘会留残影）。
- 皮肤下拉文案 {翠绿,青碧,橙色,红色,金色} 与 kSkins 严格对应；SaveFromControls 末尾 InvalidateRect 换肤即时刷新；用户词典窗 CTLCOLOR 同套（UserDict 类底 g_hBgBrush）。

**符号面板（symbolpanel.cpp）**：白底+RoundRect 16 自绘描边（去 WS_BORDER）；分类选中=hoverLight 全高药丸圆角+accent 字、未选中=kUiText2；CS_DROPSHADOW；ShowSymbolPanel 后 UiApplyRoundCorners、分类切换重设。

**快捷短语窗（hotphraseui.cpp）**：HpFont()（进程内复用雅黑 12pt，GetDpiForSystem，替代 DEFAULT_GUI_FONT）+HpBgBrush()=kUiBg（替代 COLOR_BTNFACE+1 类底）+WM_CTLCOLORSTATIC/EDIT/LISTBOX。

**工程**：`ui/LingxiUI.vcxproj` ClInclude 加 uistyle.h；全提交 `bc7be9e`（7 文件 +414/-77）。

**编译坑**：settingsui.cpp g_hBgBrush/g_hCardBrush/g_hHint 初版声明在布局块（~640 行）而 UserDict 窗（~533 行）先用→C2065；移至文件顶部 globals（g_hFont 旁）修复。

**验证**：tools\build.cmd 全绿（e2e/common/ImeCore/LingxiUI 四目标，无 error/warning）。窗体视觉无自动化断言（沿用 B2 口径），用户当时在用机器（left4dead2）未跑 UI 验证——留用户空闲时人工确认各窗口视觉。

**兼容注记**：老用户 skinIndex=0 原蓝现翠绿，无迁移（可接受）。

## 2026-10-28 晚（十三） · B6 拼音隔音符 + B7 候选窗滚轮翻页（拓展轮）

**背景**：用户"继续拓展开发"。backlog 盘点——M4.5 KV 增量（`8b5ed3b`）、R4 字号（settings candidateFontSize 12..32 即时生效）、英文句首大写（ngram `_wcsnicmp`+capitalize）、皮肤（晚·十二）均已实现，PROGRESS 旧行是文档债。选定两个真实缺口：隔音符（引擎侧 NormalizeIn/DP 边界/inOfCtx 早已预留撇号处理，但 DLL 键路由缺失使组合中按 ' 落 unmapped-default **误提交首选**——引擎支持是死代码）与候选窗滚轮翻页。

**B6 拼音隔音符（xi'an → 西安，不撞"先"）**：

- DLL（`ime/composition.cpp`）：HandlePinyinKey 在 IsLetterKey 分支后加 VK_OEM_7（'）→ 组合中无 Shift 且非双拼时 `PinyinAppend(L'\'')`（Shift+' 引号、双拼（ShuangpinToQuanpin 滤撇号隔音无意义）、空组合维持原行为）；PinyinKeepRest 字母白名单加 '（含撇号组合的部分提交剩余段不丢边界）。
- **根因修复（引擎 `ui/engine/pinyin.cpp` MatchWords）**：`xi'an` 里"先"曾以整串 cover=5 出现在候选——[xi][an] 拼接 compact="xian" 撞单音节词"先"：精确 compact 分支与模糊音分支缺音节数校验（PARTIAL 分支早有 `sylls.size()!=n` 校验，另两处漏）。补齐后：隔音输入时"先/建"类跨边界撞键词被拒（隔音语义成立）；无隔音时仅消除跨路径冗余（xian 的"西安"从 [xian] 单元路径消失、[xi][an] 路径唯一命中，行为无损）。
- **死代码修复（NormalizeIn）**：旧 `ch=='\u2019'` 窄字面量与单字节 char 比较恒假——弯撇号 U+2019（UTF-8 E2 80 99）实被整段丢弃当 xian 整串处理（旧 found=1 是 compact 撞键冗余）。改按 UTF-8 三字节序列探测（U+2019/U+2018 均归一为隔音边界），注释声称的"ASCII/弯体"支持成真；Predict inOfCtx 同款恒假条件一并清理（coverOk 保证 raw 纯 ASCII，弯体走不进该分支）。
- engine_test P6e +4 断言：`xi'an` 西安 cover=5 / 弯撇号 xi’an 西安同（cover=0——UTF-8 变长不映射，既有口径）/ 裸 xian 先 cover=4 / `xi'an` 先不整串消耗（实测 idx=-1：撞键路径全被音节数校验拒绝，比断言预期更干净）。

**B7 候选窗滚轮翻页（`ui/window.cpp`）**：WM_MOUSEWHEEL → `_pageStart` ± kPageSize 步进夹取 [0, maxStart]，翻页后 hoverRow 复位 + 锁外 LayoutAndPosition 重排。Win10+ 默认"悬停非活动窗口时滚动"把滚轮路由到候选窗（WS_EX_NOACTIVATE 宿主保焦点）；用户关闭该设置则用 PageUp/PageDown 与 -/=（DLL 侧既有）。修 C4267（size_t→uint32_t 显式收窄）。

**文档债**：PROGRESS M4.5 行 ⬜→✅（`8b5ed3b` 早已实现，旧行过时）；`common/settings.h` skinIndex 注释对齐晚·十二实际五皮肤（0 翠绿/1 青碧/2 橙/3 红/4 金）。

**验证**：engine_test 沙盒（APPDATA 隔离）功能断言全绿（P6e 新 4 条全过）；唯一红 = pinyin latency 14.3ms——归因取证 `Get-Process | Sort CPU`：left4dead2（1.5GB WS）+ deveco 多进程常驻，与晚·十一 PITFALLS #41 记录的 7~14ms 漂移同分布 = 环境非代码（B6 不触碰热路径）。tools\build.cmd 全链绿（四目标无 error/warning）。

**边界与债**：deep_test/e2e 未跑（用户用机中）——DLL 键路由（VK_OEM_7）与 UI 滚轮留用户空闲回归（代码审读口径，同晚·五 toggleMode 先例）；真实 UI 未重启（新 DLL 需重启应用生效）。学习键 NormKey 丢撇号（xi'an 与 xian 同键）——隔音选"西安"后打 xian 也置顶，可接受。

## 2026-10-28 晚（十四） · 导航键翻页与组合期接管 + strictSep 撞键校验修正

**背景**：拓展轮继续。新断言抓到上轮 compact 音节数校验的**过度拒绝回归**：`xian`（无隔音）的"西安"消失了——[xian] 单元路径对双音节词的串级匹配（compact="xian"）本是合法读法（tier-0），无条件校验把它拒成 tier-1 后被 60 候选截断。同时取证发现组合存续期**移动光标类导航键（Up/Down/Home/End/Left/Right）全部穿透到宿主**——`PinyinKeyEaten` switch 不含它们，宿主光标移出组合范围后后续 SetText 与光标错位（B5 VK_SHIFT 同族缺口，比误提交更隐蔽）。

**strictSep 修正（`ui/engine/pinyin.cpp`）**：MatchWords 紧凑/模糊分支的音节数校验改为**仅输入含显式隔音撇号时生效**——Predict 在双拼转换后按 NormalizeIn 口径（弯撇号 U+2019 同算）计算 `strictSep` 传入 Generate→MatchWords。语义：有隔音时跨显式边界的串级撞键违反用户标注拒收（xi'an 不出"先"）；无隔音时串级匹配是合法读法不校验（xian 的"西安"合法走 [xian] 路径 tier-0）。必须双拼转换后计算（ShuangpinToQuanpin 滤撇号，双拼自然无隔音）。

**组合期导航键接管（`ime/composition.cpp` + `service.cpp/h`）**：

- `PinyinKeyEaten`：PinyinActive() 时 Up/Down/Home/End/Left/Right 一律吃下（Ctrl/Alt 组合已在上方直通闸放行不受影响；PageUp/PageDown 维持 CandidatesVisible() 条件——滚动键不动光标）。
- `HandlePinyinKey`：Up/Down = 上下翻页（复用 PageCandidates）、Home/End = 首末页（新方法 `PageCandidatesHomeEnd(bool last)`，收发模式与 PageCandidates 一致：锁内夹取+版本校验+MSG_PAGE）、Left/Right = 吞下保守组合（组合光标编辑为 M5 尾巴预留，放行会造成光标错位）。无候选/已在目标页均无操作。

**P6e +2 断言**：`'an` 安 cover=3（**B6×B1 集成**：xi'an 部分提交"西"后剩余 `'an`，前导隔音边界随后续音节一并消耗，不留死撇号——cover 回放的撇号空转移 `pos++` 设计实证）；`xian` 西安 cover=4（**no over-reject 对照**：校验只拒跨隔音撞键，正是本轮回归的回归断言）。

**验证**：engine_test separator 6/6 全绿（弯撇号归一后 strict 同样生效——U+2019 转 ' 后 DP/MatchWords 口径一致）；唯一红 latency 25.4ms（left4dead2 活跃，PITFALLS #41 环境漂移，安静基线 ~7ms）；tools\build.cmd 全链绿（无 error/warning）。

**边界与债**：DLL 导航键路由仅代码审读口径（deep_test 用户用机中不跑）；真实 UI 未重启（新 DLL 需重启应用生效）。Home/End 翻页与滚轮（B7）共用 _pageStart 权威页码，无新增状态。

## 2026-10-29 晚（十四·续） · 空闲窗口全量回归 + B6 eat 侧显式接线 + deep_test T17

**背景**：用户停机开测（"我已停止使用电脑，你可以赶紧大量测试"）。先落两轮代码审读债：deep_test.exe 重建 + `tools\regress.cmd` 全量（晚·十三/十四的 DLL 键路由从未真机验证过）。

**空闲基线恢复（PITFALLS #41 归因闭环）**：regress 首轮 ALL PASS——engine_test **latency 7.156ms**（left4dead2 退场后回到安静基线，与晚·十一 7.07ms 同分布；负载期 14~31ms 漂移全归因环境成立）；fix_readings PASS；deep_test 26/26 AV=0（attempt 1/2 T10 冷启动 flake badRound=4/15，attempt 3 全净——regress 杀 UI 重启未热的已知模式）；e2e attempt 1 PASS。日志 tmp\regress-1338211878。

**probe 扩展（`ui/main.cpp`）**：ProbePinyin 输入加 L"xi'an", L"xian" 常驻诊断。实探：**xi'an→西安 rank 0**（45 cands，无"先"）；xian→先 rank 0、西安 rank 9（60 cands）——隔音语义因果对照成立。

**B6 eat 侧断线发现与显式接线（`ime/composition.cpp` PinyinKeyEaten）**：组合中 VK_OEM_7 此前落 `return IsChinesePunctKey(wParam)`——吃键依赖 cnPunct 标点映射间接达成（MapChinesePunct 的 ' → 弯引号分支返回 1）：**cnPunct=true（默认）时 B6 可用，cnPunct=false 时 IsChinesePunctKey 早退 false → 撇号整键穿透宿主，B6 handler 成死码**。晚·十三"落 default 误提交首选"的推断是错的（穿透不是误提交）。修复：switch 显式 `case VK_OEM_7`——无 Shift 且非双拼直接吃（与 HandlePinyinKey B6 闸完全一致），不依赖 cnPunct；Shift+'（引号）与双拼维持原标点/放行路径（落到 IsChinesePunctKey 判定不变）。

**deep_test T17（+3 断言，`tools/deep_test/deep_test.cpp`）**：①组合期 6 导航键（Down/Up/Home/End/Left/Right）全吃下且组合不丢（ReadFocusedText 空、后续字母仍进组合）；②空组合态负对照 VK_DOWN 放行（不误吞正常光标移动）；③B6 端到端：x,i,',a,n 全程 eaten → 空格 → 焦点文本==西安（0x897F,0x5B89）。

**验证**：build.cmd 全链绿（exit=0 无 error）；deep_test **attempt 1 即 29/29 PASS AV=0**（T17 三条全过，撇号 e2e 实证）。全量 regress 复跑：deep_test PASS + e2e attempt 1 PASS + fix_readings PASS；engine_test 174 功能断言全绿（separator 6/6、B3、cover），**唯一红 = latency 20.6ms 超阈值**——归因：笔记本**电池供电**（PowerLine Offline，平衡方案降频，纯 CPU 的词典解析 6.9s→15s 同步变慢 2.2 倍佐证），期间还排查了一段并行 StarMark `dotnet test` 干扰（~130% 单核，等其退出后复测依旧，排除）。插电基线 7.2ms vs 电池 20.6ms，用户裁定**接受电池模式结果**（功能全绿，延迟记环境值）。真实 UI 已重启（PID 7656 晚于构建，新 DLL 生效）。

**边界与债**：电池模式延迟 20ms 为环境值（插电恢复 ~7ms 口径不变，阈值不动）；组合光标编辑（M5 尾巴，Left/Right 已预留吞下）；UI 视觉人工确认（晚·十二遗留）；R3 压测收尾。

## 2026-10-29 10:5x（Loomy 续轮：测试设置沙盒根治——回归互踩事故复盘）

- 现场：HEAD 纯文档提交后 regress 全线红（engine 18 红/latency 11ms/模糊音全灭/deep T15 全红）。git log 现读+二进制时间戳比对排除代码回归，真凶=真实 HKCU 设置被历史测试刷零（PinyinEnabled=0 输入法瘫直通）——测试/用户共享注册表轨道的互踩，PITFALLS #43 立案（#32 同案并行版）。
- 根治：settings 支持 `LINGXI_SETTINGS_NS` 基键切换（白名单字符校验，Load/Save 全走 ActiveKey）；engine_test/deep_test/e2e 启动自设沙盒键+清空（缺键=结构默认=断言基线，无需预写）；regress.cmd 注入 NS 双保险。**判据验证：regress 全链跑完，真实 HKCU 前后逐字节一致 ✅**；engine/deep/e2e 同轮全绿（tmp/regress-151329693）。
- 善后：把被刷零的用户能力位恢复到正确默认（拼音/模糊/中标点/英补全/v 模式/Shift 切换=开），并以新二进制重启 LingxiUI（真实环境）。
- UI 视觉/体验重做（10-29 任务记录）**开工前置=向用户取证具体差项**，本轮未动窗体代码，遵守该闸。

## 2026-10-29 上午-中午（Loomy 续轮：用户视觉/体验投诉集中处置）

- 用户实测五条投诉全部处置：①候选窗**黑描边+屏幕残影**（CS_DROPSHADOW 方影+Win11 DWM 1px 边框线+toast 裸隐藏+异步首帧旧像素，四因分修 `b206e42`）；
②**空输入弹预测遮挡屏幕**（锚定改 caret→鼠标→记忆位三级+双轴 work-area 钳制，同提交）；③**设置页滚动频闪**（WS_EX_COMPOSITED `d8c7db1`）；
④**预测"全是同一句话的部分重复"+过长**（SplitContinuation 前缀堆叠切法重做为"短句+整句"≤2 条 `dfc9ec5`）；⑤**过拟合历史数据**（空文本预测闸+单字符缩写召回禁止，同提交）。
- 截图取证：新候选窗白卡无黑边/回显条/页码/鼠标锚定全部生效（会话内目视确认）。
- 回归：engine 175/0 全绿；deep solo 2/2 全绿；批内 T10/T14 偶发红=批负载时序噪音（#19 同族，非本轮引入，badRound 散点 11/16/18 无固定复现）。
- Win11 观感：黑边/圆角/阴影已对齐原生 flyout；更深的视觉重做仍以用户对新截图的具体反馈为开刀前提（勿盲改）。

## 2026-10-29 中午（Loomy 自走查轮：用户不当测试员，我自己来）

- 用户指令「你先自己测试不合理的地方」→ 建立自走查回路：pf_shot 假客户端触发候选窗/翻页/toast/英文续写 → 截图审 → 引擎断言复核 → 小步提交。
- 抓到并修（各一提交）：①候选窗**构建戳外泄**（改 LINGXI_BUILD_STAMP 门控 `6536e3c`）；②**回显条掐尾不掐头**（用户看不到刚敲的拼音尾——改量宽掐头保尾，并修回显行漏 SelectObject 字体 `6536e3c`）；③**首页被同音单字替换垃圾变体刷屏**（jinwantianshanghenhaode 的 进万/金丸/浸完/禁完 系——变体加词长偏好+cost 上限双闸，engine_test P4b++ 断言固化 `29f9ec2`，176/0 全绿）；④**设置整页回写刷零真实配置**（第二次复发证明产品侧有放大器——loaded 门+SaveSettingsDiff 增量落盘 `04ac2fb`，PITFALLS #44）。
- 环境事实：本机桌面被并行 agent 会话间歇占用，截图走查两次被打断——视觉复核改日补，行为级验证（引擎断言/注册表快照比对）不受影响。真实设置键已再次恢复正确默认。
- 待办移交：设置页内容超出窗口且滚动条未出现（截图可见底部按钮被裁）——settingsui 布局问题，owner-draw 是并行方地盘，先记入 LOOMY-HANDOFF 不抢改。

## 2026-10-29 午后（Loomy 续：转发疑云结案 + 观测纪律立案）

- 「--settings 转发不弹窗」追了数轮后结案：同会话内转发成功（首轮设置页截图即证据）；后续"事件不存在/进程像僵尸"全部源于 **agent 会话与用户桌面会话隔离**（Local\ 对象不可见，PITFALLS #45）。未改行为，转发链三处静默环节补了观测日志（f4911ff）。
- 纪律更新（写进 #45）：诊断常驻程序前先确认实例归属会话；用户桌面实例（宿主自动拉起）不要 kill 抢位——那是用户正在用的输入法。

## 2026-10-29 午后·额度收口

- 复核现场：HEAD e059e38 后无新提交（并行方未动灵犀）、工作区干净、用户会话 UI 正常。交接文档首部并入「额度收口快照」（现场/提交链/下轮三优先级/环境纪律），下一会话读 §0 即可开工。

## 2026-10-29 下午（新会话：AI 补全大量重复——三重去重收口 `d5a7d64`）

- **用户定性**：AI 自动推荐补全词大量重复（大问题）+ 候选词需优化 + 设置页各方面都差。本轮先收重复（high）。Loomy 15 提交已并入（HEAD 1d84514），dfc9ec5/29f9ec2 已做过一轮「去前缀堆叠/整句变体质量闸」但重复仍存。
- **取证**：ui/main.cpp 新增 `--probe-typing` 探针（逐字中文+逐词英文过真实装配链 dump 每步候选）。实证三种重复形态：①归一相等对（[0]散心 || [1]散心。）；②前缀堆叠对（[0]晴，气温20℃-30℃ || [1]晴，气温20℃-30℃，湿度70%-80%。）；③0.5B 整段复读上文（ctx"…一起去公园"→候选"天，今天天气真不错"）；另 LLM 空时词表引擎漏出 M3 占位垃圾列表（了/是/在/和…9 个单字）。
- **修复**（ngram.cpp 新纯函数 `DedupCompletions(cands, context)`，predictor.h 导出，llm.cpp Generate 多路合并后统一过闸）：①归一相等保首个；②互为前缀保较短（较长者≥2 字才合并，单字前缀"天"不足以证明同源）；③候选与上文（标点/空白断组）字符二元组重叠率 ≥60% 剔除（kill 案例 0.857/0.72，保留案例 0.12/0.29）。**WordEngine::PredictChinese 占位退役**（返回空）：中文续写 LLM 独占，无候选优于垃圾候选。
- **probe-typing 复跑**：三种形态全灭（前缀对全并/垃圾列表归零/复读被剔），英文词表补全不受影响；代价是部分步 0 候选（原本全是垃圾）——可接受。
- **regress 首跑两处测试基建红**（均为 Loomy 改动+本轮退役的叠加暴露，非功能回归）：①deep_test T14 idx pin 失效——29f9ec2 质量闸改候选序而 Loomy 只更新了 engine_test 未同步 deep_test；且序 pin 双重状态依赖（深测环境规范序 你@idx5，与开发机探针序差一位——开发机 userdict 被历史测试污染多出 泥好/呢/泥）。根治=**扫描式选词**（逐数字试探验提交字=="你"、错则 ESC 重打，彻底摆脱序依赖）。②e2e test5 Tab eaten (Chinese)——占位退役后中文候选独走 LLM，冷启动+负载超 WaitTabEaten 3s 预算，改 12s。
- **regress 二跑 ALL PASS**（engine_test 186 断言含 E8 去重矩阵 9 例、deep_test 扫描式 T14、e2e attempt 1 过）。latency 首跑 15-19ms 红=Qoder 后台负载噪音（同 binary 空闲复跑 8.9ms 过）。
- **下一优先级**：候选词优化（排序/视觉/操作/污染）与设置页重做（视觉/组织/交互/尺寸）——用户三问全选"全都要"，开工前需再取证具体样式期望。

## 2026-10-29 晚 · 用户报障大修轮（真机切换死码根治 + 一键卸载器）

用户实测反馈四项：Shift/中英切换不生效、设置页开关体感无效、词典能加不能删、设置界面锯齿；定性要求"大修"+完整应用（卸载不留幽灵输入法）。

- **真机中英切换死码根治 `e6c3d89`**：根因=TSF 真机门控——真实应用只把 OnTestKeyDown 声明 eaten 的键交给 OnKeyDown/OnKeyUp，deep_test 直调 pKm->KeyDown 绕过门控 → 切换逻辑测试全绿而真机全程死码（Shift/Ctrl 在 PinyinKeyEaten 永不声明、OnTestKeyUp 恒 FALSE，簿记与翻转都不执行）。修复四件套：ToggleKeyClaim 按 toggleMode 在 Test 阶段认领 Shift(1/2)/Ctrl(2/4)/CapsLock(3)；切换键按住期间任何其他键（含穿透键）在 Test 阶段标记 used（穿透键真机永远到不了 OnKeyDown，原和弦检测漏挡 Ctrl+Shift+K/大写字母）；OnKeyDown 对认领键统一吞下返回（不落 HandlePinyinKey——其对 VK_SHIFT 显式放行，Test 吞/KeyDown 放不一致）；OnTestKeyUp 与 KeyDown 对称认领（原恒 FALSE，宿主按消息跟踪 Shift 会滞留下压态）。OnSetFocus 复位簿记自愈丢 KeyUp。deep_test T16 断言 expectEaten false→true（4 处）。
- **词典"能加不能删"根治 `c93121e`**：引擎自学习每次选词落盘重写 userdict.txt，词典窗按列表索引删/调——用户浏览期间文件被并发重写，索引漂移后删除落空或删错；sel<0 还静默无反馈。UdDeleteSelected/UdNudgeSelected 改按 word+key 内容匹配（列表行"word    key    count"解析），失败回退索引，删除后保持选中位并明示结果（基础词典常见词仍在候选）。
- **设置界面锯齿根治（同提交）**：GDI RoundRect/Ellipse 无抗锯齿。引入 GDI+（pragma 链接 gdiplus.lib，惰性 GdiplusStartup），GdipRoundRect（GraphicsPath+SmoothingModeAntiAlias，描边 PenAlignmentInset）/GdipFillEllipse 助手替换 5 处：导航药丸、白卡（1px 描边）、开关药丸（开/关+灰边）、滑钮圆、强调色按钮。
- **一键零残留卸载器 `019fc4b`**：tools/uninstall.ps1（提权主体：语言列表摘灵犀 tips 空语言回退默认键盘 → regsvr32 /u 双位 → 皮带+背带 reg delete（CLSID x64/x86、CTF TIP HKLM/HKCU、设置键）→ SortOrder AssemblyItem 按 CLSID 枚举删（保其他输入法）→ 重启 ctfmon → 删 APPDATA 数据 → 8 项零残留自检 exit 残留数）+ uninstall.cmd（net session 检测+UAC 自重启包装）+ 托盘"卸载灵犀输入法..."入口（确认框列明动作→runas）。残留清单按 2026-10-29 实扫：HKCU CTF TIP 使能键、SortOrder 两叶子（0x0409/0x08004 的 00000001）、HKLM CTF TIP 类别树（含 CCF05DD7 历史残留）。
- **"设置无效果"排查结论**：全部设置字段消费点在册（IME 侧 pinyinEnabled/shuangpin/cnPunct/vMode/toggleMode；UI/引擎侧 paused/enCompletion/candidateCount/darkMode/llmRerank/llmMultiPath/fuzzyPinyin/traditionalOutput/symbolHotkey/skinIndex/candidateFontSize），无断链。体感主因=运行中进程载旧二进制（.prev 热替换后需重启应用/重激活 IME 生效）+切换死码放大（切换不生效→"开关没用"）。用户重启应用复测后此条待销。
- **验证**：deep_test attempt1 29/29（T16 四段切换全过）；e2e attempt2 全绿（24 PASS/0 FAIL，pass-through 'mor'/候选窗 rect/Tab 补全/中文续写/拼音 settle）。e2e 首轮三连红=用户实时测试切换 IME 抢 ctfmon 前台（foreground=other TIP），等待复跑即绿——大修期间跑 e2e 需避开用户活跃窗口。
- **坑位沉淀**（PITFALLS 大修轮）：TSF 真机 Test 门控与 deep_test 直调的偏差是"测试绿真机废"的系统性风险源；注释行尾反斜杠行拼接吞下一行声明（C2065 假报未声明）。

## 2026-10-29 深夜 · "设置不生效"真根因：沙箱变量逃逸反噬生产 + 主动补课轮

用户实机跑卸载器 8/8 零残留（完整应用闭环达成）后报"设置的大多数功能都不生效，修改无效"。

- **主动补课轮 `339184d`**：register.cmd 注册时写 HKCU Run 自启（LingxiIme→LingxiUI.exe，登录即有托盘/热键/引擎；单实例互斥锁防双开）；uninstall.ps1 对应删 Run 项+第 9 项零残留自检；设置页"中英切换键"描述明示 CapsLock 模式硬件大小写灯仍翻转；重装全周期验证（双位注册+imecheck PASSED+deep_test 29/29）。SendModeToast 链路核验：DLL→管道→候选窗光标处顶置"中/英"大字 1 秒（service.cpp:527→window.cpp:253）。
- **"设置不生效"真根因（PITFALLS #50）**：`LINGXI_SETTINGS_NS` 沙箱（#43）的逃逸方向是反的——deep_test/e2e 进程内的 TSF DLL 按需 spawn 生产 LingxiUI 时继承测试环境，**生产设置 UI 带着沙箱变量启动**，用户 19:40 的全部 17 项修改写进 `LingxiIme_test` 沙箱键；引擎（真实应用进程）读真实键永远读不到。取证三板斧：注册表键 LastWriteTime 对齐事件时间线（真实键 19:31:22=deep_test pin、沙箱键 19:40:31=用户保存、userdict 19:40:39=用户在打字）+ 读活进程环境块（NtQueryInformationProcess→PEB）确认幸存实例干净 + 真实键仅剩 pin/restore 孤儿值（ToggleMode/ShiftToggle）反证保存侧断链。
- **修复**：① ui/main.cpp 生产守卫——probe 分发后、单例前强制 `SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", NULL)`（probe 子命令保留沙箱，e2e 断言依赖；生产 UI 永远读写真实 HKCU）；② deep_test PinToggleMode/RestoreToggleMode 改走 ActiveKey 同口径沙箱键（旧实现直写真实键，违反 #43 自己宣称的"真实 HKCU 从此不共轨"）；③ 用户 19:40 的 17 项设置从沙箱键迁移回真实键（ToggleMode=4=用户最后选择的"仅 Ctrl"）。
- **验证**：deep_test 29/29 且真实键 LastWrite 全程零变化（pin/restore 落沙箱键 20:03:32）；真实键 ToggleMode=4 幸存；LingxiUI 重启后进程环境块无 LINGXI 变量。
- **用户侧说明**：设置即时生效（引擎每按键实时读注册表）；explorer/WT/Qoder/webview2/MSPCManager 等 09:56-10:22 启动的应用载着晨间旧 DLL——设置项对它们同样生效（注册表共用），但本轮切换/删词新功能需重启应用加载新二进制。

## 2026-10-29 夜 · 对标微软拼音：差距文档 + 被遗忘需求清算（候选词右键词典操作 + 符号面板扩充）

用户点名三项：①输出完整对标微软拼音的差距文档；②清算被遗忘的需求——词典增删/自定义排序/**候选词右键排序**；③特殊字符/emoji 补全。

- **代码审计（文档素材，全实测锚点）**：pinyin.cpp 简拼支持完整（INITIAL 单字母/混合模式 2^n、用户词简拼索引 _userByInit、纯声母整词路径、ShuangpinToQuanpin:108 小鹤双拼）；模糊音 5 组；NormKey:813=小写 a-z 滤除；Learn:923（双拼先转换再 NormKey）；LoadUserDict:825 不重置 _userDirty 但重载自文件→UI 写盘后无丢失。引擎架构确认：引擎在 LingxiUI 进程内，DLL 只发 MSG_CTX，UI 算候选回 MSG_CANDS。
- **①候选词右键词典菜单**：settingsui.h 导出 `CandDictOp{Pin,Boost,Lower,Delete}` + `UserDictHasWord` + `ApplyCandidateDictOp`；window.cpp 加 WM_RBUTTONUP（命中测试同 LBUTTONUP 口径=回显行偏移+CurrentPageRangeLocked；SetForegroundWindow+TrackPopupMenu TPM_RETURNCMD|TPM_NONOTIFY 托盘同款模式；菜单置顶/提升/降频/删除，inDict 灰显降频删除）。词典操作锚定**词内容**不锚定索引（引擎并发重写文件）；键推导=InferPinyinKey 优先（与用户输入方式无关，双拼组合串是键位序列非全拼），失败回退 keyHint 仅全拼模式；失败 LX_LOG 不弹窗（候选窗上下文不抢焦点）。**坑**：实现初放匿名命名空间内→LNK2019，移到 `} // namespace lingxi` 前修复（PITFALLS #51）。
- **②符号面板扩充**：symbolpanel.cpp 码点模型改**字符串数组模型**（支持颜文字多字条目+VS16 变体选择符 ✌️🖐️❤️）；5 类→**10 类**（常用/数学/货币/箭头/表情/手势/动物/食物/^_^ 颜文字，各 24-38 条）；Category 含 cellW/cols（普通 38×12 列，颜文字 104×4 列 GridX() 居中），kCatTabW=48 面板宽 488。
- **③差距文档 docs/GAP-VS-MSFT.md**：9 节——被遗忘需求清算表（4 项全✅）、基本输入/中英切换/候选选字/词典学习/符号特殊输入/智能特性/工程集成 7 大矩阵（✅🟡❌+P0-P2+代码锚点）、路线图 R1(本轮)-R6（R2=缩写短语映射、R3=日期时间直出、R4=拼音纠错、R5=emoji 候选融合）。基于代码实测现状，不凭印象。
- **构建+回归**：build.cmd（x64）✓、build32.cmd（Win32 ImeCore）✓、deep_test 重建 ✓。deep_test Run B 9/29 三 attempt settle 全红（probe_q_eaten=0）→锁定 `_pinyinMode false→TestKeyDown 放行`（composition.cpp:603）→复跑 Run C：settle 3/3 过，attempt 3=28/29（唯一红 T10 连击时序压测 badRound=16，用户活跃期典型干扰），T0-T17 产品用例安静窗口全过 AV=0——确认 9/29 为环境干扰非代码回归（用户活跃期实时切 IME 抢 ctfmon 前台，与 #19/#49 同源）。
- **用户侧说明**：右键菜单/符号面板全在 LingxiUI 进程——运行中实例（20:39 新二进制）**无需重启即可测**；托盘菜单或 Ctrl+Shift+B（需 symbolHotkey=1）开符号面板。

## 2026-10-29 深夜续 · 深度回归轮：日志取证揪出 DetectPinyinMode 定时器泄漏并根治

用户质问"你难道忘了要深度测试"→ 全套 regress.cmd + 带日志取证 + 修复取证中暴露的真 bug。

- **全套回归**（tmp/regress-2922522603）：engine_test 186 断言绿、仅时延红（median 16.1ms>10ms 预算，#41 后台负载——deveco 4967s CPU+LingxiUI LLM 活跃，空载基线 8.9ms）；fix_readings PASS；**e2e attempt 1 绿**；deep_test 干扰期红。**#43/#50 硬判据**：回归前后真实 `HKCU\Software\LingxiIme` reg export 逐字节一致（零污染），沙箱键仅 ToggleMode/ShiftToggle 残留。
- **LINGXI_LOG=1 取证锁定真 bug（PITFALLS #52）**：DetectPinyinMode "放弃"分支只打日志不 KillTimer——SetTimer 是周期定时器，预算耗尽后仍以 250ms 节拍永久空转（干扰期 97 秒刷 390 行风暴），宿主进程每秒 4 次 TSF 查询泄漏；同时干扰期 attempt 日志 **0 条 [key] 行**=探键根本没到本 DLL（前台被其他 TIP 持有，#49 同源环境干扰）。
- **修复**（service.h:220 `_modeSlowRetry` 标志 + composition.cpp:220-248 DetectPinyinMode 耗尽分支）：放弃→**降级 1s 慢速自愈轮询**（SetTimer 1000ms、降级日志 log-once、premature 每拍仅进程内 GetForeground µs 级查询），前台回归后 ≤1s 自愈进位；正常路径 KillTimer 完全静止。选降级而非 KillTimer 硬停：负载机 ctfmon 过渡可超 2s 快速预算，硬停会把 settle 假红引进来。
- **验证**：x64+Win32 双构建绿；修复后带日志 deep_test 日志仅 28 行（降级日志恰 1 条、旧"放弃"0 条、无风暴）；**deep_test attempt 2 = 29/29 全绿 exit=0**（attempt 1 用户活跃 fgIsLingxi=0，内部重试壳按设计工作）。**终验（用户配合安静窗口）**：e2e exit=0（attempt 1 typing 已过仅 1 项冷启动时序红，attempt 2 全绿）；engine_test exit=0 全绿（拼音时延 median 9.727ms<10ms 预算，证实白昼 16.1ms 红=后台负载 #41）。四套（engine/fix_readings/deep/e2e）全绿收口。
- **工具坑（本条随记）**：`cmd /c '"含空格路径.exe"'` 的引号转写在 PS 5.1 下偶发剥引号（报 `'D:\Visual' is not recognized`，且会掩盖"文件根本不存在"的真相）——原生 exe 直接用 PS 调用操作符 `& '完整路径'`，或先 Test-Path。engine_test 实际产物在 `bin\enginetest\`（非 tools 下 bin\Debug）。

## 2026-10-29 深夜终 · GAP §8 路线图三连发：R4 拼音纠错 + R3 日期时间直出 + R2 自定义短语

深度回归收口后按路线图连做三个功能（各带 engine_test 断言组+小步提交），注入链统一设计：**自学习置顶重排 > 自定义短语 > 日期时间 > 常规**（习惯优先于便民、显式配置次之）。

- **R4 拼音纠错 `d9c109e`**：文件级 `CorrectSyllableTypo`——仅当子串非合法音节时尝试改写且命中 `_syllables` 才生效：①韵母全写→缩写（iou→iu/uei→ui/uen→un）；②gn→ng 颠倒；③丢 g 补全（末尾 n→ng）。rawLen=原串长保证 cover 回放；Extend 罚 5（正常 FULL=2、简拼=4），合法切分永不劫持（`ren` 等不动；yingnuo 类改写形不命中音节表自然出局）。engine_test P10：8 误拼用例（niou→牛/shuei→水/chuen→春/shegn→生/magn→忙/zhon→中/din→定/son→送）+ren 不劫持全 PASS。
- **R3 日期时间直出 `e6d551a`**：文件级 `BuildDateTimeCands`——整串触发 rq/sj/xq/rqsj（GetLocalTime 实时；rq=年月日/-//三格式、sj=HH:MM/HH:MM:SS/上午下午 12 时制、xq=星期N/周N、rqsj=日期+时间）+ `InjectExplicitTop`（注入去重前插置顶、cover=0 全清语义、截断 kPredictMaxCandidates）。触发串 `inTrig` 在**双拼转换前**计算（NormalizeIn 口径；双拼 rq/sj 键序本无解，触发是纯增益）。P11：4 触发+不劫持(rqa)+备选格式全 PASS。
- **R2 自定义短语 `b41109f`**：引擎 `_phrases` 表+`LoadPhrasesLocked`（缩写<TAB>短语，UTF-8 BOM 容忍，缩写归一小写 2..16 位纯字母，短语≤200 字符），与用户词典同通道热载（LoadUserDict 尾部同锁装载）；UI 新增 phraseui（仿 hotphraseui：缩写/短语编辑+列表+增删校验+原子写，settingsui 词典页入口 IDC_BTN_PHRASES=120，main 绑 SetPhrasesChangedCallback→ReloadUserDict）。P12：整串命中/大小写归一/短语高于日期时间/加长不劫持/删文件热卸载全 PASS。
- **顺带根治 e6d551a 注入顺序缺陷**：R3 提交时 dt/phrase 注入写在 ups 锁块**之后**但拷出在锁内——注入执行时 phraseCands 恒为空，R2 引擎侧首版照抄同结构复现同一缺陷（注入点先于锁内 `_phrases.find` 拷出）。修复：ups 锁块（含 _phrases 拷出）整体前移至注入之前，单次加锁语义不变；注入后 ups 重排天然保 learned 第一。
- **时延踩线红的 A/B 判据**：P12 轮 engine_test 拼音时延两连红（11.9/13.5ms>10ms）——worktree checkout HEAD 基线（R3 提交时 9.6ms 绿）同负载复测 **18.9ms**：基线比工作区更差，确证环境负载（atrustagent 84%+双 deveco 实例 39/37%，#41）非代码回归。此后时延红先 worktree A/B 再下结论，不盲改热路径。
- **验证**：engine_test 全绿（P10/P11/P12 全 PASS，唯一时延红 A/B 判环境）；deep_test attempt 3 **29/29** exit=0（attempt 1/2 T12 翻页抖动，内部重试壳按设计工作）；docs/GAP-VS-MSFT.md 路线图 R2/R3/R4 标已完成。

## 2026-10-30 · 路线图收尾两连发：R5 emoji 候选融合 + R6 屏蔽词（40 分钟快轮）

R2-R4 收口后用户要求继续低难度功能、40 分钟内收口——选 R5+R6（都复用既有注入/热载基建）。

- **R5 emoji 候选融合 `fd7bfa2`**：文件级 `EmojiFusionMap`（静态 35 词条：哈哈→😄/谢谢→🙏/恭喜→🎉…选词原则=多字稳定词优先，避开"对/错"类高频单字霸屏）；Predict 去重循环后融合——emoji 紧随触发词（同 cover，提交替换等长拼音），只跟随不夺位、同 emoji 多触发词 seen 去重一次、截断 kPredictMaxCandidates。静态表零配置零文件零热路径负担（一次哈希查找/候选）。P13：谢谢→🙏 紧随/nihao 无映射页干净/哈哈→😄 全 PASS。
- **R6 屏蔽词 `7a5077b`**：`%APPDATA%\LingxiIme\blocked.txt`（UTF-8 BOM 容忍，每行一词≤50 字符，上限 500 条），LoadUserDict 通道同锁热载（LoadBlockedLocked）；Predict 锁内一次拷出（通常空集），**去重循环与 ups 自学习注入两处同口径过滤**；显式意图（dt/短语/emoji）不受影响——用户显式配置优先。文件驱动 MVP（改文件+重启/ReloadUserDict 生效），管理界面留待后续。P14：屏蔽"谢谢"后 Generate+emoji 融合同灭/删文件热卸载回归首选+emoji 恢复全 PASS。
- **测试细节**：P14 屏蔽词用 UTF-8 字节直写（`\xEF\xBB\xBF\xE8\xB0\xA2…`）避开 P12 作用域内 wToUtf8 lambda 复用；P13 首版"无映射检查"误用 xiexie 结果（mapped 输入）而非 nihao 独立预测，编写时自查抓获。
- **验证**：engine_test P13/P14 全 PASS（时延红 17ms 同 #41 环境负载——atrustagent/deveco 持续占用，R5/R6 热路径增量=一次空集锁拷+一次哈希查找/候选，µs 级）；deep_test **attempt 1 = 29/29** AV=0（优于上轮 attempt 3）；e2e 未跑（引擎候选追加/过滤，不触 TSF/协议层，deep_test+engine_test 已覆盖）。
- **工具坑复发确认**：Start-Process 重定向 stdout 时子进程（LingxiUI/deep_test 派生）继承管道写端致 -Wait 挂起，输出已完整写出后才被超时杀壳——判读以输出文件内容为准，勿误判测试挂死。

## 2026-10-30 深夜 · 用户报障两连修（v 模式吞字母 + 简拼拟声垃圾）+ 选字链路性能根治

用户报两 bug：①v 模式后按字母被吞；②简拼 zm 首选出"在呣"类拼装词。修复过程中 deep_test 暴露 T10/T14 回归（Learn 持锁拖垮 Predict），一并根治，三连提交收口。

- **v 模式直通+悬账 `6368930`**：弃缓冲式（_inlineComp=false 下字母无显示、英文续写断裂）改直通——v 触发只置 `_vDebt` 悬账，字母经宿主原生上屏免费获得显示与续写；首个字母键消费悬账进 vbug 语义，退格撤销、Esc 取消、其他键补写 v 再放行，DetectPinyinMode 复位清账。deep_test T15 四块（i/u/in/iu 直通、悬账消费、退步撤销、双出口）全绿。
- **拟声音节加罚 `f469ed1`**：根因=m/n/hm 等无元音叹词注音在音节表合法，FULL 成本 2/格使 [z][m:FULL]=6 压过正常简拼链 [z][m]=8。IsInterjectionSyllable 判定加罚 cost 5（>简拼 4/格），单独打 m/n 呣/嗯 仍出字。engine_test P15 四断言（zm=怎么/zn 无垃圾/mu=55 en=25/mm=妈妈）全 PASS。
- **T10/T14 根因=Learn 持锁盘写+全量重建饿死 Predict（PITFALLS #53）**：时间戳取证（log.h 本轮加 HH:MM:SS.mmm 前缀）实测 ver=118 SendCtx→候选回包 **464ms**——选字后 NotifyPicked→Learn 在管道读线程持 `_udMtx` 跑 SaveUserDictLocked（全量盘写）+BuildUserInitIndex（~200 键×EnumeratePaths≈300-600ms），Predict 同锁被饿；空格等待 80ms 超时退化 PinyinCommitRaw 提交裸字母"hao"。空闲基线 9.6ms 无感，重载机（Predict 30-156ms）越预算。
- **修复双管 `75e4ee9`**：①Learn 锁内只做内存更新+简拼索引单键增量（UpdateUserInitIndexLocked，保持"新词即时可简拼调出"同步语义），磁盘落盘移后台 detached 线程（KickPersist，_persistBusy CAS 防管道读线程/UI 拾取回调两入口并发，快照即清脏、写失败恢复脏位）；②提交键等待预算 80→400ms（空格/数字/标点默认/CommitChinesePunct 四处同口径，标点两处原无等待补齐）——候选就绪零等待，最坏键延迟优于提交错误文本。
- **测试适配异步落盘**：engine_test Learn 用例重载实例前轮询 userdict 等后台写完成、恢复备份前等喵喵落盘落地（防迟到异步写覆盖备份）。
- **验证**：engine_test 全绿（Learn 置顶/重载持久化/喵喵/mm 简拼、P15）；LINGXI_LOG=1 deep_test **attempt 1 = 31/31** AV=0（T10 badRound=-1、T14 digit=6 final=你好 noLetter=1）；日志复测 SendCtx→回包 **max 280ms**（原 464ms）p95 210ms avg 91ms——400ms 预算覆盖最坏值留余量。
- **UI 侧日志缺失之谜（PITFALLS #54）**：deep_test 拉起的有 env LingxiUI 实例总被单例弹掉——taskkill 后用户其他持有 DLL 的 app 1 秒内抢 respawn 无 env 实例占住单例；如需 UI 文件日志须让 env 实例先占单例（或杀 orphan 宿主）。

## 2026-10-30 午后（Loomy 续：转发疑云彻底结案——观测坑第二层）

- 「--settings 转发不弹窗」真结案：窗口**一直正常创建并显示**（EnumThreadWindows 实锤 LingxiSettingsWnd vis=True）；此前所有"查无窗口"都是 **FindWindowW 跨 window station 观测假象**（PITFALLS #55，#45 同族第二层）。转发链无 bug。
- 排查中捞到的真实防御缺口已修：CreateSettingsWindow 复用路径不校验句柄存活——若窗口曾被销毁，转发将永远静默空转。改为 `g_hSettings && IsWindow()` + 失效句柄置空重建（带日志）。
- 遗留两项视觉验证（设置页滚动真值/勾选 checked 渲染）：桌面被并行会话 StarMark 活跃占用，遮挡不可靠取证——移交下轮桌面空闲时做（窗口枚举方法已改用 EnumThreadWindows）。

## 2026-10-30 午后（Loomy 自主推进：R3 压测收口 + 模糊音组细分）

- R3 遗留压测做完：pf_ux 三轮 63 样本，p50=62ms / p95=187ms / p99=453ms（冷缓存尾），打字 CPU 4%、零抖动——数字入 GAP §10，"真机 p50/p95"待办销账。
- GAP R6 长尾项开工：**模糊音组细分开关**（`ef28cda`）——fuzzyGroups 位掩码（缺键=全组历史无感）、ComputeFuzzy 组门控、Predict 热载自动重建索引、设置页 5 行子勾选（一组未勾=退回全组防误操作）、engine_test P8b 门控 4 断言；regress 一次 ALL PASS（engine 218/0）。
- 过程小坑：多行补丁脚本 CRLF/LF 锚点错位一次（改行级 splice 修复）；settingsui 全局声明误插逗号列表中间（重定位）。均编译门禁捕获，未逃逸。

## 2026-10-30 午后·续（Loomy：候选窗透明度定制）

- GAP §3 透明度 ❌→✅：candidateOpacity 设置（70–100 四档，缺键=100 与历史一致）→ CandWindow 创建加 WS_EX_LAYERED、Show 时 ApplyOpacity 按 % 去重应用 LWA_ALPHA → 设置页「外观」页不透明度下拉（复用 combo 模式）。
- 验证：全链构建绿；regress 一次 ALL PASS（engine 218/0+deep+e2e，tmp/regress-257628887）；**程序化实测** GetLayeredWindowAttributes 返回 ok=True flags=0x2(LWA_ALPHA) alpha=255（100% 档正确输出，通道打通实锤——桌面多 agent 争用中，截图不可信时改用窗口属性取证，#55 方法论延伸）。
- 过程：沙盒 NS 实例设 80% 触发候选窗验证时实例被宿主重启换岗（读到的是真实 NS 实例 255）——不影响链路结论，档位映射是同一 MulDiv 纯计算。

## 2026-10-30 午后·续2（Loomy：模糊音扩到 8 组，GAP §1 🟡→✅）

- 承接组掩码框架顺手扩 f/h、l/r、g/k 三组（bit5-7）：ComputeFuzzy 三新分支、设置页 8 行子勾选、默认掩码 0x1F→0xFF（缺键行为仍=全组，老用户无感）、Load 校验放宽到 0xFF。
- engine_test P8b 追加 f/h 双向断言（关组 fu 不出"湖"、开组可出）；总 220/0；regress 一次 ALL PASS（tmp/regress-886520942）。
- GAP §1 模糊音行 🟡→✅（组数与微软持平且细分粒度不输）。

## 2026-10-30 午后·续3（Loomy：透明度+8 组模糊音收口，联想根因钉死并定稿设计）

- 候选窗透明度四档 ✅（`ef0e61d`，LWA_ALPHA 通道程序化实测打通：flags=0x2 alpha 正确）；模糊音扩到 8 组 ✅（`7569b31`，GAP §1 模糊音 🟡→✅ 组数持平微软，engine 220/0）。
- **"没有自动补全感"的代码级根因钉死**：#24 的 `_fSelfInsert` 守卫（本为符号插入设计）顺带杀死拼音上屏后的联想链路——中文选词提交后无任何下一词建议。
- 定稿设计写入 GAP §11（CTX_F_ASSOC 显式请求绕开守卫；联想只鼠标点选、任何按键即撤销**不劫持键盘**——比微软保守；settings 默认开+可关；三层验证方案），工作量 ~80 行，下轮 P0 照图施工。

## 2026-10-30 深夜·续4（Loomy：选词联想 P0 落地全链路，空格连续输出达成）

**`04d589b` 一笔入魂（8 文件 268 行）**：按 GAP §11 照图施工 + 两处施工期发现。

- **DLL 侧**：`SendAssocCtx` 在 non-inline/inline 两处全量上屏后显式发联想帧（复用
  SendPinyinCtx 同款编辑会话读光标前文+caret，绕开 #24 `_fSelfInsert` 守卫）；
  `_assocReq/_assocShown` 回包认号，`OnTestKeyDown` 撤销闸，点选联想后连发下一轮
  （传送带体验）。设置 `assocAfterPick` 默认开（Load/Save/Diff 三面同权）。
- **UI 侧零改动**：联想帧=普通非空上下文帧走现成链预测——比设计预估还小一圈。
- **施工期关键发现=空格静默黑洞（PITFALLS #56）**：空组合候选窗下空格被
  IsSelectionKey 排除在外 → Test 放行 → TSF 分发 OnKeyDown 不到 → 键蒸发、
  深度测试两次"旧 exe 假绿"叠加掩盖（deep_test 不在 sln，build 静默旧产物）。
  修正：联想态专属认领空格 + HandlePinyinKey 空组合联想选词道复用点选路；
  数字键经既有三面门走同一条道——"可空格连续输出"达成且不伤英文 ghost。
- **T18a/b 真机绿灯**：attempt1 33/33（jintian→空格→联想段连出；联想窗打 hao
  →接管出"好"）；ASSOC 帧 120 发/35 全回包系统对账。透明度/8组模糊音同日回归位。
- **现场**：regress-2937629603 ALL PASS；真实环境 UI 收口即恢复。

## 2026-10-30 深夜·续5（Loomy：regress build gate 根治旧 exe 假绿，engine Release 基线坐实）

**`ee8e810`（+134/-97）**：tools/regress.cmd 插 build gate——跑批前强制重建 sln(ImeCore/LingxiUI/e2e)+deep_test(Debug)+engine_test(Release)，任一构建失败 exit 1，绝不带旧二进制开测；LINGXI_SKIP_BUILD=1 审计后门。#56-B 的坑在下一次"继续"里当场再犯（两批 deep 断言计数不变=14:59/16:30 旧 exe 复读），本刀永久堵死。

- **gate 首跑战果二件**：① engine 被以 Debug 重建后 latency 17ms 全红——摸出"构建配置=断言语义"，engine Release 才是历史基线（README/HANDOFF/cur cl 诊断旁证齐全），gate engine 行改 Configuration=Release 后终验 ALL PASS；② vcxproj 配置侦察的反查串写错差点误判"无 Release 配置"，现读 `<ProjectConfiguration Include=` 全量纠正。
- 终验（regress-100829655）：build sln/deep/engine 三行在列，engine_test.exe mtime 随批刷新（09:54:48Z=17:54）=真重建硬证；engine PASS+fix_readings PASS+deep PASS+e2e attempt1 PASS → **RESULT ALL PASS**；沙盒 APPDATA 模拟独立复跑 engine 16KB/exit0/0FAIL 双确认。OUT 归档个别 0 字节=cmd detach 重定向缓冲偶发，行为级证据以主日志+时间戳为准（另案观察）。
- **本回合纪律插曲**：工具通道混入多段伪造执行史（假 diff/假构建通过/假提交号），全程以现读字节与独立复跑裁决，未采信任何转述。
- 现场：真实 UI 恢复（24164 @17:59:27）；HKCU 真实键逐条完好；tmp 本回合脚本清零。
- 现场：用户真实环境 UI 已恢复（透明度/8 组即时可用）；本轮 regress 多次一次 ALL PASS。

## 2026-10-30 晚（Loomy：待办1 两项视觉验证双收口 + 设置窗 SB_TOP/SB_BOTTOM 补缺）

**待办1-① 设置页滚动条真值/底部裁切：PASS，无缺陷（10-29 走查疑虑结案）**
- API 真值：WS_VSCROLL 在窗；GetScrollInfo min=0 / max=1076 / page=641 / pos=0；窗口 900x680、client 867x641。
- 真屏（CopyFromScreen）+ PrintWindow(flag 2) 双证：track RGB(240,240,240)、thumb RGB(133,133,133)，位置与渲染一致。
- WM_VSCROLL SB_PAGEDOWN pos 0→435 到底，底部态尾部行（第 8-14 行）完整可达渲染——**底部无裁切**。
- PrintWindow 黑条（右缘 x892-899、左缘、底缘）= DWM resize 边框伪影：应用不画该区域，PrintWindow 输出纯黑，真屏无——**非缺陷**（PITFALLS #58）。

**待办1-② owner-draw 勾选 checked 态：PASS（验证对象改判：现 UI 已无 checkbox，布尔项全部为 mkToggle 药丸，BS_OWNERDRAW + 外置 g_toggleOn，settingsui.cpp:1113-1145）**
- ON 态：accent #107C10（默认翠绿皮肤）药丸——常规页控制区 9 条绿带（y158-656，行距 60px）、底部态 7 条、智能页选词联想 1 条。
- OFF 态：浅灰 pill(234) + 1px 描边(130) + 深灰钮(90,90,90) 居左——智能页 LLM 重排/多路采样、常规页小鹤双拼/繁体输出实测。
- 全宽扫描 y95-112 绿带确认为导航选中项左侧绿竖条（非控件区）；无黑块/错位/默认 Win32 样式；外置状态双向渲染正确（排除"g_toggleOn 损坏致全绿"疑虑）。

**顺带修复：WM_VSCROLL 处理器缺 SB_TOP/SB_BOTTOM case（ui/settingsui.cpp:1316-1318）**
- 症状：程序化 Home/End（SB_TOP/SB_BOTTOM）无效；用户路径（滚轮/拖拽/PgUp/PgDn）不受影响，故长期未暴露。
- 修复后两轮 tools/build.cmd exit 0；对运行中设置窗实测 SB_BOTTOM 0→435、SB_TOP 435→0、PAGEDOWN 0→435 全对。
- **本轮未跑全量 regress（当时）**：改动仅设置 UI 两行 case；机器持续重载（atrustagent 84%）+用户活跃窗口在场，build×2 + 运行进程实证已覆盖本改动面；**收口提交后当即补跑：`tools\regress.cmd` 一次 ALL PASS**（tmp/regress-2538823020：build gate 三构建 + engine + fix_readings + deep + e2e attempt1 全绿）——缓跑账清零。

**竞态插曲（PITFALLS #57）**：期间两次非本会话启动的 LingxiUI（12816@18:33:04 / 22912@18:50:58）；12816 为竞态旧码进程（SB_TOP 无效暴露，实为加载修复前 exe），22912 加载含修复 exe 实测通过。判定一律以"进程 StartTime vs exe LastWriteTime + 新 case 实测"为准，勿以 Get-Process 焦点进程为先验。
- 方法论沉淀：截图取证一律 PrintWindow(hwnd, hdc, 2)（抗遮挡窗口干扰）；PS EnumWindows 委托回调内 Write-Output 不外传——必须 ArrayList 闭包（引用类型）收集；FindWindowW 按类查在本环境多次失败（#55 同族），EnumWindows+闭包可靠。
- 取证产物：tmp/verify/（脚本群 + settings-print/bottom/screen/smart/final.png）留档供复核，gitignore 内不入库。

## 2026-10-30 深夜（Loomy：记事本光标停滞根因修复 + 全项目深度分析）

**用户报障**：Win11 记事本中上屏文字后光标停留原位，后续输入插到旧光标处。

**根因（PITFALLS #59）**：TSF 规范对 `InsertTextAtSelection` 后的选区位置零承诺；DLL 从不 SetSelection；全部测试宿主宽松（插入自推选区）掩盖缺陷；Win11 记事本（WinUI CoreText 适配层）严格不动选区 → 光标停滞。replaceLen 污染排查排除（中文路径恒 0，仅英文补全 ngram.cpp:147 用 prefix.size()）。

**修复（ime/editsession.cpp）**：
- CInsertSession：InsertTextAtSelection 成功后 `Collapse(TF_ANCHOR_END)` + 显式 `SetSelection`（TF_AE_NONE，失败仅日志）——宽松宿主同位幂等，严格宿主补上前进。
- CReplaceSession：SetText 成功后同法推光标（英文补全 Tab 路径）。
- 内联组合死代码路径不动（`_inlineComp` 恒 false，最小爆炸半径）。

**测试建模（tools/deep_test/deep_test.cpp）**：
- TestStore 严格化（模拟记事本）：SetText/InsertTextAtSelection 不再动选区；GetSelection 防御性夹取；插入点夹取。
- 宿主侧自理：文件级 g_pStore + ResetSelection()；ClearFocusedText 清空后重置选区 (0,0)（防陈旧选区越界 _buf.insert out_of_range）。
- 新增 T19 光标契约：T19a 上屏后选区折叠于文本尾（sel={2,2}）；T19b 连续上屏顺序+前缀+选区三元断言（sel={3,3}）；本地 hex19 lambda 副本（#56-C 纪律）。
- e2e 保持宽松宿主（第二风味覆盖）；e2e.cpp [DIAG] 诊断挪到 fail-fast 前，失败现场（store/focused/文档指针）永久可见。

**证据链**：A/B in-vitro 复现三轮确定——A（旧 DLL+严格 store）T14/T18a/T18b/T19a/T19b 五红（"好你"顺序错乱/联想作废读空上下文/sel={0,0}）；B（新 DLL）35/35 全绿。提交路径无旁路确认（CInsertSession/CReplaceSession 仅 service.cpp:615/:632 两构造点）。

**regress**：一次 ALL PASS（tmp/regress-494731470：build gate 三构建 + engine + fix_readings + deep(严格面含 T19) + e2e attempt1）。

**e2e 假红事件（PITFALLS #60）**：19:47-58 负载噪声窗口 e2e 六连假红（pass-through 读空，3 attempt × 2 轮全同症状）；A/B/A 交替实验制造"二进制相关"假象；逐字节 diff 两版 DLL 仅 7 字节 PE 元数据差异（物理矛盾即证伪）；安静环境复跑同二进制直接绿。方法论：同源二进制"确定性分叉"先假设环境噪声，Get-FileHash+字节 diff+安静复现三步裁决。

**全项目深度分析**：docs/DEEP-ANALYSIS.md（同类风险面/测试矩阵/架构债/取证方法论）。要点：选区契约面闭环；残留同类风险仅内联死代码（建议下轮删除专项）；P2 LINGXI_LOG 默认关致零 in-vivo 证据；P3 用户字典双写/路径双实现待复查；记事本人工抽测 SOP 建议。

## 2026-10-30 夜二（Loomy：六项报障修复 + 引擎垃圾墙/长简拼双根因收口）

**用户报障六项**：#1 v 键无输出、#2 toast 期间打字候选不弹、#3 补全窗漂移、#4 右键菜单异常、#5 垃圾候选+翻页、#6 符号面板关不掉。外加 probe 复测暴露的 gylgl 长简拼全灭（用户主诉"这后面会影响一堆问题"）。

**六修复**：
- **#1 vMode 退役**：settings vMode/settingsui 勾选/service `_vMode` 全删；v 走 PinyinStart 普通组合（"v" 非法拼音→引擎 0 候选→echo 回显；空格 CommitRaw 原样上屏 "v"/"vlog"）。真机 TSF 分发"Test 已吃→KeyDown FALSE 不保证回传"是旧悬账机制失灵根因（composition.cpp:936 注释）。
- **#2 toast 定时器吞候选**：ShowModeToast 设 1s 自动消隐 timer(1)，WM_TIMER 到点整窗 SW_HIDE；toast 后紧随打字的候选帧被吞。修=Show()/SetPage() 入口 KillTimer(1)。
- **#3 补全窗漂移**：CReadSession GetTextExt 用上下文范围 pBack（kCtxMaxChars 段 bbox）定位，长上下文锚到段首。修=改用插入点选区 pRange（editsession.cpp:45-54，职责分离：pBack 只管读文本）。
- **#4 右键**：候选窗 WM_RBUTTONUP 补菜单，不加 SetForegroundWindow（保持 NOACTIVATE）。
- **#5 垃圾候选+翻页**：垃圾墙根因=残段池+逐字硬拼（"嗯西啊"类三字拼接）。修=Generate 单一 tier 闸（tier!=0 路径仅发词典词，allInit 且 n-span>1 跳过；span==n 放宽 kPredictMaxCandidates、前缀碎词限 4）+候选数设置化（CandidateCount=9）+OnCandidates rerank 保页。probe 实证：un=59（嗯系绝迹、五年保留）、xian=60（西安居首、西啊嗯绝迹）。
- **#6 面板三出口**：热键重按切替关闭（旧实现只置顶纹丝不动）+头部 × 显式出口（NOACTIVATE 收不到 Esc）+右键关闭。

**引擎双修（PITFALLS #61/#62）**：
- **v-INITIAL 排除**：单字母简拼把 v 当合法声母→[v][k-PARTIAL] 死头路径残段池发 k 声母字（vk→可）。修=简拼条件 `in[i] != 'v'`。T15 v+k+space='vk' 恢复；jv/qv/xv 降级 CommitRaw（边角记录）。
- **longAllInit 近全跨度发射**：HEAD 遗留 `allInit && n>=4` / `prefixAllInit && initHead>=3` 两处整条 continue 杀掉一切 ≥4 声母纯简拼（gylg 0 候选→echo 小窗被感知"消失"；gylgl 无供应链管理）。修=longAllInit 标志+tier 闸前受限发射（span n-1..min(6,n)，span==n limit 60、span<n limit 4、uncovered 罚 8/音节）。probe 实证：gylg→[供应链|供应量|郭云龙|感应炉]、gylgl→[供应链管理]、xyzxyz→0、gyl=28 不回归。
- **engine_test P4c 阈值 40→20**（PITFALLS #63）：40 是垃圾墙时代基线，健康基线 23，取 20 留余量。

**验证状态**：probe 全基线 7 输入对照通过（nichifanlemawohenb=23/v/vk/vlog/gyl/gylg/gylgl/xyzxyz）；engine_test 重建单跑 0 FAIL（tmp/engine_test_manual.txt）；deep_test PASS + e2e attempt2 PASS（tmp/regress-2404128378）。**三套件全绿 regress 留待用户停机后**：现役宿主 PID 24504 是 21:52 二进制（无 22:16 longAllInit）且带 test-NS split-brain（PITFALLS #64），须重启后跑。

**静态审计（用户用机期间，无交互测试）**：tier-0 ①②③④ 全段（④ all-init 头部闸与 longAllInit 顺序正确，拼接垃圾不复活）；EnqueuePredict 每客户端最新+全局有界；SendPinyinCtx 无长度截断；键分发 Shift/CapsLock 放行语义；SendHide 六处调用点语义（PinyinResetNoInline/部分提交收尾/失焦/空帧均正确）；symbolpanel 三出口+网格边界守卫；editsession #3 复核；WM_TIMER 仅 toast 定时器。**无新缺陷**。遗留疑点：0 候选 echo 窗"消失"感待真机定性（gylg 场景已根治，v/vk 场景停机后看）。

**git 状态**：15 文件 dirty（HEAD=16e3e6c），待停机后全量回归绿再分批提交。

## 2026-10-30 夜三（Loomy：候选窗工具条大版本——左键右键/按钮/拖拽全上）

**用户需求**：继续优化 UI 与候选词框，尽可能多加功能，左键右键都用上，加按钮也行。

**底部工具条**（window.cpp，替代原 +18 静态页码条）：
- **‹ 页码 › 翻页按钮**：可点翻页（首/末页灰化），与滚轮/键盘翻页同夹取语义；页码居中显示。
- **固/符/设 三按钮**（右组）：固=位置固定开关（生效中强调色点亮）、符=符号面板（与托盘/热键同 g_reqSymbols 路）、设=设置（g_reqSettings 路）。
- **按钮 tooltip**：悬停 400ms 自绘小浮窗（懒建常驻隐藏复用，跨线程只 ShowWindow 不 DestroyWindow——避开跨线程销毁陷阱）；含用法提示（如"设置（右键换肤）"）。换按钮时重挂 TME_HOVER（一次性事件，不重挂则 tooltip 只出第一次）。
- **光标反馈**：按钮手型（IDC_HAND）、拖拽手柄四向箭头（SIZEALL）——WM_SETCURSOR 全客户区分区。

**拖拽移动 + 位置固定**：
- 拼音回显行/工具条空白=拖拽手柄，按住移动窗口（SetCapture+光标偏移），松手即把当前位置写入固定设置（注册表持久化，重启宿主仍生效）。
- 固定模式（candidatePinned=1）LayoutAndPosition 不再跟随光标，钳制到所在显示器工作区（拔显示器/改分辨率不丢窗）。
- 固左键=切换、固右键=解除；toast 期间禁拖拽（防把固定坐标写到 toast 小窗位置）。

**右键菜单增强**（候选行）：新增"复制该词"（CF_UNICODETEXT 剪贴板，失败静默）、"联网搜索该词"（Bing，UTF-8+百分号编码中文安全进 URL）。**设右键=循环换肤**（5 皮肤即时生效，免进设置页）。

**设置新增**（settings.h/cpp 三字段三处读写）：CandidatePinned/CandidatePinX/CandidatePinY（int 负值经 DWORD 往返——多显示器左侧屏）。SaveFromControls 从 LoadSettings 起+diff 落盘，新字段不被设置页保存冲刷 ✓。

**接线**（main.cpp）：wnd.SetButtonHandler → g_reqSymbols/g_reqSettings（Btn 枚举 Symbols/Settings 转发，翻页/固钉窗口内部自理）。

**构建**：build.cmd 全绿（初版 drawBtn lambda const RECT* 编译错，按值传 RECT 修复）；--probe-pinyin 冒烟全基线不回归（gylg=4 候/gylgl=供应链管理/v 系 0/nichifanlemawohenb=23/xian=60 西安居首）。**视觉验证待宿主重启后**（现役宿主仍是旧二进制）。

### 交互二（同夜续：功率键批）

用户要求继续自主加功能，选纯增量可静态验证批：

- **中键点击候选 = 置顶该词**（WM_MBUTTONUP；免开右键菜单，与菜单"置顶"同 ApplyCandidateDictOp(Pin) 路；toast 期间守卫）。
- **Ctrl+左键候选 = 联网搜索、Shift+左键候选 = 复制**（WM_LBUTTONUP 修饰键分支，默认左键选词语义不变；word 在同一锁内解析，剪贴板/浏览器操作在锁外）。
- **截断候选行悬停 tooltip 显示完整词条**：行宽 = `_width-2*pad`、文本 = `"N␣␣词"` 与 OnPaint 同口径测宽，超出才弹（复用按钮 tooltip 常驻窗）。**换行重挂 TME_HOVER**（每行独立 400ms 停留，与按钮 hover 同型）——否则 tooltip 只出首行。
- **右键拼音回显行 = 复制拼音串**（词空 + pt 在回显行判定；回显行本无词典菜单，补上专属动作）。
- **托盘菜单新增"固定候选窗位置"勾选项**（ID 2007）：勾选态读 candidatePinned；切换走新回调 SetPinToggleCallback → main 绑 `wnd.TogglePin()`（按窗口实际位置写入，settingsui 无位置信息的解法）。CandWindow::TogglePin 与"固"按钮左键同语义。
- 接线：settingsui.h/.cpp（g_reqPinToggle + setter + 菜单项 + case）、main.cpp（绑定）、window.h/.cpp（TogglePin + 四个交互点）。
- 构建全绿；probe 冒烟基线不回归（gylg/gylgl/xian/nichifanlemawohenb 全保持）。

### 交互三（同夜续：托盘/滚轮批）

- **开机自启动**：托盘菜单"开机自动启动"勾选项（ID 2008）。状态**以 HKCU\Run\LingxiIme 键为唯一真源**（不另存设置，杜绝双源漂移）；开=写引号包裹的 GetModuleFileNameW（带空格路径必需），关=删值。**发现**：该键已存在（早期 setup 写入 `tools\..` 路径形式）——同名同目标，IsAutoStartEnabled 正确识别为已开启，开关行为兼容；重开会改写为干净路径。
- **托盘悬停文字状态化**：暂停时 tip 变"灵犀输入法（已暂停）"（AddTrayIcon 建立时按设置定、托盘"暂停预测"切换后 NIM_MODIFY 刷新 UpdateTrayTip）。设置页无暂停开关（托盘是唯一路径，无需额外挂钩）。
- **Ctrl+滚轮=候选字号 ±1**（12–32 钳制）：先于翻页判定占位（Ctrl 按住时滚轮不翻页）；即时 RebuildFonts+LayoutAndPosition+写设置。设置页字号控件开着时不同步（g_updating 既有护栏，重开页即读新值；与设右键换肤同已知限制）。
- **鼠标侧键翻页**：WM_XBUTTONUP 后退/前进键=上/下一页（DoPageStep 同夹取），return TRUE 防重复导航语义。
- 构建全绿；probe 基线不回归。

### 交互四（同夜续：帮助窗 + 符号"最近"页）

- **使用帮助窗**（托盘菜单 2009）：只读多行 EDIT 速查页，集中列出全部交互（候选窗手势/修饰键/工具条/符号面板/托盘/词典）——功能越加越多后的可发现性兜底。WM_CTLCOLORSTATIC 修只读 EDIT 灰字；字体 static 复用防反复开关泄漏；重复打开复用已开窗。
- **符号面板"最近"页**：
  - settings 三件套加 `recentSymbols`（std::wstring，\x1F 分隔、最新在前、≤12 项）：GetString/SetString（REG_SZ，2KB 上限、NUL 填充剥离）+ Load/Save/Diff 三处；settings.h 首次引入 <string>（DLL 共用无碍）。
  - symbolpanel：`CatAt(i)` 访问器（0=动态"最近"，1..N=静态分类）、`kTabCount=kCatCount+1` 全部触点改造（标签命中/绘制循环/宽度/×按钮区/网格）；点击符号 AddRecent（去重前移+SaveSettingsDiff+正在看"最近"页即时刷新）；空态提示"暂无最近使用"（PanelHeight 保一行高）；打开时 LoadRecents+智能默认标签（有记录开"最近"，空则开"常用"）。
  - 指针稳定性：g_recentStore（wstring）存活期内 g_recentPtrs（c_str）稳定，每次变异后 RebuildRecentPtrs 再重绘。
- **坑**：初版 RegGetValueW 传 8 参（正确 7：hkey/subkey/value/flags/type/data/cb）——编译期抓出；PS 5.1 嵌套引号调 vcvars+msbuild 解析失败，改写 tmp\build_engine.cmd 脚本文件执行。
- **验证**：构建全绿；engine_test 单跑 **220 PASS / 0 FAIL**（test-NS 沙盒，settings.cpp 字符串路径过全套）；probe 基线不回归。

### 交互五（同夜续：滚轮换页 + 帮助快照 + 设置备份）

- **符号面板滚轮换分类**：WM_MOUSEWHEEL 上滚=左一页（11 页横向点击不便的快捷手势；取模环绕、随页重设尺寸/圆角/重绘——与标签点击同路径）。
- **帮助窗实时配置快照**：帮助文本尾部追加"【当前生效配置】"段（切换键/皮肤/候选数/字号/透明度/模糊音/双拼/繁体/补全/标点/联想/热键/LLM/固定/自启动/暂停——打开时刻快照，只读）。数组索引取自 Load 钳制后的值（toggleMode≤4、skinIndex≤4）不越界。
- **托盘设置备份导出/导入**（2010/2011）：reg.exe export/import 整个 HKCU\Software\LingxiIme 键（全部设置+热短语；用户词典在 APPDATA 不含——词典窗自带导入/导出）。CreateProcessW 隐藏窗口 15s 超时取退出码；导入后 g_settingsChanged（热键重注册）+ UpdateTrayTip；导入确认弹窗防误覆盖。**reg export 命令语义 headless 实证通过**（1360B 备份文件）。
- 词典标记（候选词前用户词典小圆点）评估后放弃：UserDictHasWord 每调用整读+解析词典文件，paint 内 9 行×每帧 hover 重绘 = 每次悬停 9 次文件 IO，成本不成比例。
- 构建全绿；probe 基线不回归。

### 交互六（同夜续：皮肤扩充 + 备份完整化 + 剪贴板可靠）

- **皮肤 +2**：茶棕（112,76,44）/玫粉（186,26,79）——白字对比度足够（亮度 ~83/~80）；触点全改：uistyle.h kSkins、settings.cpp 载入钳制 4→6、settingsui 组合框填充+bound 4→6+帮助窗名字表、settings.h 注释。
- **备份完整化**：导出默认文件名带日期（LingxiIme-设置备份-YYYYMMDD.reg）；**用户词典随行**（同 basename"-用户词典.txt"，词典在 APPDATA 文本文件、注册表备份外）；导入检测随行词典→确认恢复→引擎 reload。备份从此单文件迁移完整。
- **剪贴板占用重试**：OpenClipboard 被其他进程短暂占用（剪贴板锁）时 3×30ms 重试——复制是用户主动点击，偶发占用不该静默失败。
- **评估放弃**：托盘快捷短语子菜单（点托盘时焦点已离开宿主文本框，SendInsertText 无处落地——热键路径焦点不动所以可行，托盘路径语义错误）；词典窗条目计数（已存在"已学习词条：N"）。
- **验证**：构建全绿；engine_test 单跑 **220 PASS / 0 FAIL**；probe 基线不回归。

### 交互七（10-31 零点半：11 项微功能批）

1. **Shift+滚轮**：候选窗不透明度 ±5%（70–100 钳制即时生效+写设置；与 Ctrl+滚轮字号同款"修饰键占位滚轮"）。
2. **右键工具条空白 = 皮肤直选子菜单**：7 款勾选直选（设右键循环换肤保留为盲切快捷；同款不抢前台）。
3. **符号面板网格悬停高亮 + 手型光标**：HitItem 三路复用（点击/hover/SETCURSOR），TME_LEAVE 清态，换页清高亮。
4. **符号面板"日期"页（第 12 标签）**：日期/年月日/时间/时分秒/星期/组合 6 项，开面板与切页现算（时间保鲜）；cellW 210×2 列（"2026年10月31日"≈200px）；不进"最近"（随开随算+长串挤坏 38px 格）。
5. **托盘"暂停 30 分钟后自动恢复"**：SetTimer id=2 到期恢复预测；手动切换暂停态（2004）KillTimer 防止陈旧定时误恢复。
6. **托盘"重新加载用户词典"**：g_reloadUserDict 直调（外部编辑词典文件后免重启）。
7. **托盘"打开数据文件夹"**：取 UserDictPathW 父目录（与实际数据位置恒一致），CreateDirectory 兜底 + explorer 打开。
8. **托盘中键 = 使用帮助**：左键设置/右键菜单已占，中键空闲常驻。
9. **托盘"恢复默认设置..."**：ResetSettingsToDefaults（RegDeleteTreeW 删键，LoadSettings 走"键不存在=全默认"；沙箱感知 ActiveKey）→ 热键重注册+tip+控件同步；词典/短语在 APPDATA 不受影响。
10. **词典操作浮窗反馈**：右键菜单四操作与中键置顶成功后 1.5s 浮窗"已置顶：×××"（复用 tooltip 窗 + 独立定时器 id=2——与 toast 的 id=1 分离，到期只关浮窗不藏候选窗；失败仅日志）。
11. **--version**：三级输出（重定向管道→UTF-16LE 直写；父有控制台→AttachConsole+显开 CONOUT$——AttachConsole 不更新 std 句柄，直接 WriteConsoleW(hOut) 在重定向场景写管道必败；双击→弹窗）。
- **验证**：--version 重定向路径实证（exit=0 + UTF-16 输出正确）；构建全绿；engine_test 220 PASS / 0 FAIL；probe 基线不回归。
- **遗留**：12 项全部为 UI/托盘侧，真机交互验证（hover 高亮观感/日期页换页刷新/30 分钟定时恢复）待 12:00 后统一走查。

### 交互八（10-31 凌晨：20 项功能批）

用户继续要求自主加功能，选 20 项纯增量批（候选窗手势 / 符号面板扩容 / 托盘状态 / 命令行 / 帮助窗三件套）：

1. **候选行手型光标**：WM_SETCURSOR 行区返回 IDC_HAND（点选语义可发现性；按钮/拖拽区分区不变）。
2. **右键菜单"查该词拼音"**（id 7）：InferWordPinyin 从 settingsui 导出（settingsui.h 声明 + 底部 InferPinyinKey 包装），浮窗 2.5s 显示结果。
3. **Ctrl+Shift+左键 = 删除该词条**：与菜单"删除"同 ApplyCandidateDictOp(Remove) 路；浮窗反馈（keyHint 传 L""——WM_LBUTTONUP 处无 composing 变量，编译错修复点）。
4. **中键修饰键功率组合**：默认=置顶 / Shift=降频 / Ctrl=删除（mop/mfb 变量分发，原裸中键置顶升级为三合一）。
5-8. **符号面板 4 新静态分类**：注音（31 个 ㄅ-ㄩ+声调）/ 希腊（48 个 α-Ω）/ 框线（38 个 ─┌┐└┘├┤┬┴┼╬ 等）/ 序号（32 个 ①-⑳Ⅰ-Ⅻ）——插在 ^_^ 前，kCats 现 14 项（kCatCount=14、kTabCount=16 自动驱动面板宽度）。
9. **右键"最近"标签 = 清空记录**：WM_RBUTTONDOWN y<kCatH 判定，仅"最近"页清空（其余标签右键仍关闭面板——关闭语义不回归）。
10. **记忆上次静态分类**（lastSymbolCat uint32_t 三处读写）：WM_DESTROY 落盘 [1..kCatCount]（动态页 0 最近/日期不记忆，各有更优默认），开面板恢复否则智能默认。
11. **托盘 tip 状态摘要**：BuildTrayTip 统一构造——暂停/拼音关/双拼/繁体逐项追加后缀，AddTrayIcon 与切换点全走同一函数。
12. **备份含热键短语随行**：导出同 basename"-热键短语.txt"；导入检测→询问→恢复→NotifyHotPhrasesChanged 重注册。hotphraseui.h 新导出 HotPhraseDataPath/NotifyHotPhrasesChanged（SaveHotPhrases 自动触发回调，外部改文件后须手动 Notify——不能直调 SetHotPhrasesChangedCallback 换回调）。
13. **--help**：kHelpText 命令行清单（--version/--help/--diagnostics/--probe-*）。
14. **--diagnostics**：BuildDiagnosticsText——OS build/UBR/DPI/exe 路径/设置摘要/词典行数/短语条数（先清 LINGXI_SETTINGS_NS 防沙箱污染）。
15. **日志轮转**（log.h AppendToFile）：>1MB 先 DeleteFileW(.old) 再 MoveFileW 改名留档重开——长期开 LINGXI_LOG=1 防无界增长；wchar_t 数组拼路径（log.h 不含 <string>，避免引入依赖）。
16. **设置页记忆**（lastSettingsPage uint32_t 三处读写）：导航点击 SaveSettingsDiff 落盘；CreateSettingsWindow LoadToControls 后恢复（越界回首页——页数随版本变）。
17. **Ctrl+Alt+P 全局热键切暂停**：kTrayHotkeyPause=3（settingsui.cpp，按窗隔离不与 main.cpp 的 id 冲突），InitTrayAndSettings 注册、MainWndProc WM_HOTKEY 处理、WM_DESTROY 注销。
18. **帮助窗 WS_EX_TOPMOST**：速查窗不应被候选窗压住。
19. **设置窗居中打开**：CreateWindowEx 后按鼠标所在显示器工作区 SetWindowPos 居中（CW_USEDEFAULT 级联位观感差）。
20. **帮助窗 Ctrl+滚轮字号 12–28**：g_helpFont/g_helpFontH 提升文件级（原 static 局部），HelpWndProc WM_MOUSEWHEEL 重建字体。
- **EmitCliText 共享输出**（main.cpp）：--version/--help/--diagnostics 共用四级输出——①控制台句柄（GetConsoleMode 判定）WriteConsoleW；②管道 WriteFile UTF-16LE；③AttachConsole 后显开 CONOUT$（AttachConsole 不自动更新 std 句柄）；④MessageBox 兜底。
- **kHelpText 全量同步**：中键修饰键/Ctrl+Shift 删除/4 新分类/Ctrl+Alt+P/帮助窗 Ctrl+滚轮/命令行清单（settingsui.cpp 与 main.cpp 两个 kHelpText 常量分别同步）。
- **核查确认已存在/刻意不加**：候选行 hover 高亮（window.cpp 758-763 已有）、‹/› 禁用态（drawBtn disabled 参数已有）、CS_DROPSHADOW（Win11 方角投影残影根因，142 行注释刻意）。
- **验证**：构建全绿（修一处编译错：WM_LBUTTONUP 无 composing 变量）；--help/--diagnostics 重定向实证 exit=0（诊断输出 OS 26300.9711 26H2/词典 441 条）；engine_test 单跑 **220 PASS / 0 FAIL**；probe 基线不回归；**日志轮转实证通过**（seed 1,200,000B → --probe-pinyin exit=0 → .old 恰 1,200,000B + 新日志重开，tmp\test_rotation.ps1）。
- **遗留**：真机交互验证（手势/新分类/热键/字号缩放）随交互六/七一并待 12:00 后统一走查。

### 交互九（10-31 凌晨：11 项功能批）

用户继续要求自主加功能。核查排除已存在项（darkMode 三态设置/加载/使用全链路早已完整——候选清单先查代码再动手）。

1. **托盘"暂停 1 小时后自动恢复"**（ID 2016）：与"30 分钟"（2012）共用 SetTimer id=2 同一到点恢复路径，写作/演示临时静音加长档。
2. **Ctrl+Alt+H 全局热键打开使用帮助**（kTrayHotkeyHelp=4）：注册/WM_HOTKEY/WM_DESTROY 注销三触点，与 Ctrl+Alt+P 同型同容错（被占用仅记日志）。
3. **帮助窗底部"复制全部文本"按钮**：HelpCopyAll——正文（含实时配置快照）一键进剪贴板（CF_UNICODETEXT，剪贴板被占 3×30ms 重试）；按钮文字反馈"已复制/复制失败"，WM_TIMER id=3 1.5s 复位。
4. **帮助窗 Esc 关闭**：窗口本体 WM_KEYDOWN 处理 + **正文 EDIT 子类化**（GWLP_WNDPROC 存旧proc、CallWindowProcW 转发——焦点在 EDIT 时按键不冒泡到父窗，必须子类化；沿用 hotphraseui 热键录制框同款模式）。
5. **设置窗 Esc 隐藏**：与 WM_CLOSE 同语义（先存矩形再隐藏）。
6. **界面字体族选择**（uiFont 0-3：微软雅黑/宋体/等线/Segoe UI）：uistyle.h 新增 kUiFontFaces/kUiFontNames；window.cpp RebuildFonts 按设置取 face（小字默认 Segoe UI，非默认族跟随保持观感一致）；Show() 族变化即时重建（与字号/皮肤同款"读设置去重"路径）；设置页外观页"字体"下拉。
7. **设置窗位置/尺寸记忆**（settings setWinX/Y/W/H 四 int，负值经 DWORD 往返）：隐藏三路径落盘（WM_CLOSE / Esc / 托盘左键切换）+ WM_DESTROY 兜底，IsIconic 最小化守卫；创建时恢复，MonitorFromRect+IntersectRect 校验（显示器拔了/分辨率变了回退居中默认）——与"重开回 900×680 居中"的旧观感告别。
8. **用户词典窗搜索过滤框**（IDC_UD_SEARCH）：列表上方 EDIT，EN_CHANGE 实时过滤（词条/拼音键子串命中，不区分大小写）；计数行过滤态显示"匹配 N / M 条"。删除/升降频按内容匹配不受过滤影响。
9. **--diagnostics 设置摘要补字体字段**（字体=%u）。
10. **帮助快照补两行**（界面字体/明暗——darkMode 此前一直缺席快照）；**cfg 缓冲 1600→4096**：kHelpText 历次增补后已逼近截断线（_TRUNCATE 静默截尾风险），本轮一并扩容。
11. **kHelpText 全量同步**：暂停 1 小时 / Ctrl+Alt+H / 【帮助窗/设置窗】节（Esc、复制、矩形记忆）/ 词典搜索行。
- **验证**：构建全绿一次过；engine_test 单跑 **220 PASS / 0 FAIL**（test-NS）；probe 基线不回归（gylg=4 候/gylgl=供应链管理/xian/nichifanlemawohenb 全保持）；--diagnostics 重定向实证 exit=0（输出含新 字体= 字段，用户实况 字号12/不透明度70% 正确呈现）。
- **遗留**：真机走查（复制按钮/搜索过滤/热键/矩形恢复/字体切换观感）待 12:00 后与交互六/七/八一并。

### 交互十（10-31 凌晨续：打字统计批，6 项）

先探查后动手：--version/恢复默认（2015）/数据文件夹（2014）/词典导入导出（304/305）/模糊音分组 UI/翻页/rq-sj 日期直出 均已存在，排除；**打字统计为真缺口**。

1. **打字统计核心**（新文件 ui/usagestats.h/.cpp，进 LingxiUI.vcxproj）：今日/累计选词次数、上屏汉字数（CJK 基本区+扩展A+兼容表意）、活跃天数、连续使用天数（纯 YYYYMMDD 日期回退算连续，无时区问题）；计数点=PipeServer::ApplyLearn（UI 点选与 IME 数字键选词的公共落点，cover 调整前计数）；懒加载；每 20 次选词节流落盘 + 退出（消息循环后）+ WM_ENDSESSION 补写，崩溃最多丢 20 次。
2. **沙箱自动停用**：LINGXI_SETTINGS_NS 非空（deep_test/e2e 拉起的测试宿主）→ 全部读写停用，回归跑分不污染真实统计；--diagnostics 路径 main.cpp 先清 NS 再读，统计恒真实。
3. **打字统计窗**（托盘 2017 / 设置·词典页"打字统计"按钮行）：只读 EDIT 正文 + 底部按钮条（刷新/清零（确认框，不可恢复）/导出诊断报告）；Esc 关闭（窗口+正文 EDIT 子类化，同帮助窗双路）；TOPMOST、工作区居中、字体进程内复用。
4. **导出诊断报告**：诊断全文（\n→\r\n）+ 统计正文 → APPDATA\LingxiIme\lingxi-diag-YYYYMMDD-HHMMSS.txt（UTF-8 带 BOM 记事本直开），成功/失败 MessageBox 反馈 + 日志。BuildDiagnosticsText 由 main.cpp 自由函数改经 settingsui.h 全局声明供统计窗复用。
5. **帮助/诊断同步**：BuildHelpText 快照 + "打字"行（BuildStatsBrief）；--diagnostics + "打字统计"行；kHelpText（settingsui 版）托盘菜单列表补"打字统计" + 新增【打字统计】节。
6. **vcxproj**：usagestats.cpp/.h 挂入 LingxiUI 工程（无 .filters 文件，直接主工程）。
- **验证**：构建全绿（修一处警告：BuildHelpText 变参 BuildStatsBrief 缺 .c_str()）；**播种实证**——手写 usage.txt（12/34/5600/21000/45）→ --diagnostics 重定向 exit=0 输出"打字统计：今日选词 12 次/34 字，累计 5600 次/21000 字，活跃 45 天"精确回读（懒加载+容错解析+摘要格式全链路），验后播种文件已删；probe 基线不回归（gylg=4 候/gylgl=1 候/xian/nichifanlemawohenb）；engine_test **220 PASS / 0 FAIL**。
- **遗留**：真机走查（统计窗观感/清零确认/导出文件/选词计数累计）待 12:00 后与交互六/七/八/九一并。

### 12:00 收口（10-31 午：全量回归 + 真机走查 + 分批提交）

用户停用电脑，执行 tmp/plan_1200.md 收口。

1. **全量回归 ALL PASS**：cmd /c tools/regress.cmd 一次通过（engine_test 220 / fix_readings / deep_test / e2e；regress-1439217053）。
2. **真机走查 smoke1（15/15）**：就绪门（Local\LingxiUI_ShowSettings 事件可开=托盘/热键/转发链全就位，替代固定 Sleep——冷启动 LLM 加载 8.5~27s 波动）；--settings 转发开设置窗；设置页 IDC_BTN_STATS=132 开统计窗；播种值（5600/21000/45）经 WM_GETTEXT 回读；清零确认框（GetDlgItem IDOK）+ 复位；导出诊断报告（文件+含"打字统计"节，验后删）；Esc 关统计窗；词典窗 111 开+搜索过滤（440→2）+计数行"匹配"；Ctrl+Alt+H keybd_event 注入开帮助窗+Esc 关闭。
3. **真机走查 smoke2（8/8）**：设置窗矩形记忆——移动+缩放（587,315,870x660）→WM_CLOSE 隐藏落盘 HKCU SetWin* 四键精确回读→taskkill 重启宿主→--settings→GetWindowRect 精确恢复→清理基线。
4. **排障收获（PITFALLS #65/#66）**：smoke 首跑 12 项 FAIL 逐层剥洋葱——①`FindWindowW(cls, $null)` 的 `$null` 被 PS 封送成空串（#37 同族，改 EnumWindows+GetClassNameW 按类枚举）；②无 BOM .ps1 中文字面串静默乱码（#12 静默变体，FindWindow 中文标题/Contains 全部假失败——加 UTF-8 BOM 后全绿）；③taskkill 宿主后 7 进程内的 ImeCore DLL 管道客户端 1 秒内重生宿主（无 LINGXI_LOG），带日志实例静默出局——"宿主没日志"假象根因；④跨进程子控件文本 GetWindowTextW 不可靠，改 SendMessage(WM_GETTEXT)。**转发链/设置窗/统计窗代码本身零缺陷**，前几轮"设置窗未创建"结论全部为工装误判。
5. **残留核查**：usage.txt/lingxi-diag*/SetWin* 测试值全清；用户词典 440 条完好（10-30 21:38 后未触碰）。
6. **分批提交**：26 文件（24 改+2 新）按 core→ui→docs 三批（文件在交互六~十间大量重叠无法按交互切分；ImeCore 依赖 pinyin.cpp 必须随 core 同批）。

## 2026-10-31 夜（用户睡眠期授权自主重构：P1→P2→P3 三段落地）

依据 docs/ARCH-ASSESSMENT.md §5 路线推进；纪律：**纯代码搬移、公共 API 不变、每段 build+regress+真机 smoke 三门禁全绿才提交**，三段独立提交保持回滚粒度。

### P1 拆分 settingsui.cpp（`eda3083`）
- 2868 行 → 6 文件：settingsui.cpp（设置窗 1796）/ tray.cpp（托盘+主窗 384）/ userdictwnd.cpp（用户词典窗+词典存储 748）/ helpwnd.cpp（帮助窗 288）/ statswnd.cpp（统计窗 221）/ settingsbackup.cpp（设置备份 155）。
- 新增 ui/uiinternal.h = 拆分后内部共享面（跨文件符号声明；各文件私有状态留匿名命名空间）——只限拆分文件族使用不外泄。
- 门禁：build Debug 零错；regress ALL PASS 一次过（tmp\regress-53168902）；真机 smoke1 15/15 + smoke2 8/8。

### P2 拆分 window.cpp（`9e3d411`）
- 1542 行 → 3 文件 + 1 内部头：candwnd_core.cpp（生命周期/状态翻页/固钉/toast/LayoutAndPosition/WndProc 分发壳 427）、candwnd_render.cpp（kBuildStamp/RebuildFonts/ApplySkin/LayoutMetrics/ToolbarHeight/ShowTip/HideTip/s_TipWndProc/OnPaint 407）、candwnd_interact.cpp（TipTextFor/CopyTextToClipboard/OpenWebSearch/HitTestButton + 10 个 On* 手势 744）、candwnd_internal.h（Dpi/Scale96/CalcToolbarRects，static→inline）。
- WndProc 十个鼠标 case 体逐字提取为私有 On* 方法（window.h 仅增 10 条私有声明，公共 API 零改动）；OnMouseLeave/OnMouseHover 补 hwnd 参数（方法体内 InvalidateRect/tme.hwndTrack 需要）。
- 核验：tmp/verify-p2.ps1 大括号匹配提取器对照 git show HEAD 原文 **40 块逐字 PASS**（唯一预期差异：OnSetCursor 尾 break→return FALSE）。
- 门禁：build 零错；regress ALL PASS；smoke1 15/15 + smoke2 8/8。

### P3 回调收口 UiHooks（`4118113` + e2e 加固 `20b9f9f`）
- 6 组"文件级全局 + SetXxxCallback"归并为 uiinternal.h `struct UiHooks` + `g_hooks` 单结构（定义 settingsui.cpp；**唯一写点=main.cpp 装配段一次性填充**，消费点统一读 g_hooks.xxx）：reloadUserDict/showSymbols/settingsChanged/pinToggle（原 settingsui.cpp:825-828 四全局）+ phrasesChanged（原 g_phChanged/phraseui.cpp:32）+ hotPhrasesChanged（原 g_hpChanged/hotphraseui.cpp:35）。
- 6 个 setter 全退役（settingsui.h×4、phraseui.h、hotphraseui.h 声明与各自 cpp 定义删除）；BuildDiagnosticsText 声明由 settingsui.h 全局命名空间迁 uiinternal.h lingxi 命名空间（main.cpp 定义加 lingxi:: 限定，statswnd.cpp 调用点零改动、settingsui.h include 顺手移除）。
- 顺带去重：phraseui/hotphraseui 匿名 ns 私有 ReadFileBytes/WriteFileBytes 与 userdictwnd.cpp 共享实现逐字等价（原子 .tmp+MoveFileEx）——include uiinternal.h 后重载歧义 C2668 暴露，删私有副本转共享实现。
- **e2e 门禁三连红→真修**：foreWnd=Chrome_WidgetWin_1 实为 IDE（ZCode）持前台，无前台权的 e2e 宿主 SetForegroundWindow 静默失败 → AssociateFocus 不绑定 → focusDoc=NULL（PITFALLS #67）。修 tools/e2e/e2e.cpp ForceForeground（AttachThreadInput 抢前台），修后同环境 attempt 1 直绿。独立提交 20b9f9f。
- 门禁：build 零错；regress ALL PASS（tmp\regress-1102421052）；smoke1 15/15 + smoke2 8/8。
- 收尾：宿主恢复运行态（bin\Debug P3 新二进制 + 真实设置 ToggleMode=4/CandidateCount=9/DarkMode=1/FuzzyGroups=0xff 完好无污染）；usage.txt 为 smoke1 播种→清零口径残留（既有测试行为，与前两段门禁一致）。

### 备注
- 工装沉淀：tmp/verify-p2.ps1（括号匹配逐块核验器，可复用后续拆分段）、tmp/smoke1.ps1（15 项）/tmp/smoke2.ps1（8 项）继续作为真机门禁。
- 三段净效果：ui 层最大文件 2868→1796（settingsui.cpp）、1542 行 window.cpp 消失（3 文件均 <744）、9+ 散装 setter 收敛为 1 结构体 1 写点；公共头（window.h/settingsui.h）与 DLL/引擎零改动。
## 2026-10-31 · R6 屏蔽词管理窗（`4981be0`）

GAP-VS-MSFT §8 R6 的管理界面留白项：引擎热载（`7a5077b` P14）早已就绪，本轮补可视化编辑，**引擎零改动**。

- **新文件 ui/blockedui.{h,cpp}**（phraseui 模板，AGENTS.md 纪律 1：新窗口=新文件）：列表 + 词输入 + 添加/删除选中/清空（带确认）+ 状态行；窗口类 LingxiBlockedWnd，控件 ID 段 600-606（ID 空间：settings 101-132 / userdict 300-313 / help 350 / stats 360-362 / hotphrase 400-406 / phrase 500-506 / blocked 600+）。
- **口径镜像引擎 LoadBlockedLocked**：词 1-50 字符、上限 500 条、UTF-8 带 BOM、去重（unordered_set）；保存走共享 WriteFileBytes（原子 .tmp+MoveFileEx）→ fire `g_hooks.reloadUserDict()`（blocked.txt 与 userdict 同 _udMtx 同通道，LoadUserDict 尾部装载）。
- **入口双路（settingsui.cpp 零增长，P0 上界 1796 行不破）**：托盘菜单 2018"屏蔽词管理..."（tray.cpp）+ 词典窗按钮行 7→8 加"屏蔽词"（IDC_UD_BLOCKED=313，userdictwnd.cpp）。
- **备份随行**（settingsbackup.cpp）：导出同 basename"-屏蔽词.txt"；导入检测→询问→恢复→fire reloadUserDict（与用户词典/热键短语同模式）；日志加 bl=%d 字段。
- **帮助同步**（helpwnd.cpp）：托盘菜单清单加"屏蔽词管理"、词典与学习段加两行说明。
- **行为探针取证**：blocked.txt 写入"郭云龙+西安" → probe gylg 4→3 候（郭云龙消失）、xian 首选 西安→先；删除文件后基线复原（gylg=4 候/gylgl=供应链管理 1 候/xian→西安）。
- 门禁：build 零错；regress ALL PASS（tmp\regress-1420324283）；smoke1 15/15 + smoke2 8/8。
- 工装注意：PowerShell 跑 LingxiUI.exe（GUI 子系统）**不等待即返回**，probe 日志读到旧内容——行为探针须 `cmd /c` 包裹（等子进程退出）；PS 5.1 无 `` `u{...} `` 转义，BOM 靠 UTF8Encoding(true) 即可，勿在字符串里手拼 FEFF。

### R6 闭环：候选右键直接屏蔽（`281d692`）

管理窗（`4981be0`）之后的功率键补齐：看到垃圾候选右键即屏蔽，免开窗。

- **blockedui 公共 API 三入口**：`BlockWord`（校验+去重+原子写+fire g_hooks.reloadUserDict，`BlockWordResult` 五态枚举：Blocked/AlreadyBlocked/LimitReached/TooLong/SaveFailed）/`UnblockWord`/`IsWordBlocked`——热载封装在 blockedui.cpp 内，candwnd_interact 只 include 公共头 blockedui.h，不触碰 uiinternal.h（P0 纪律 4/5）。
- **右键菜单 8 号项**：按当前屏蔽态切换文案（未屏蔽="屏蔽该词（不再出现）"；已屏蔽="取消屏蔽（恢复候选）"+MF_CHECKED），结果统一浮窗 1.5s；超限/写失败各有文案。
- **管理窗 BlAdd 复用 BlockWord**：校验/去重/落盘/热载与右键同一条路，switch 五态映射状态行文案——口径唯一定义点。
- 门禁：build 零错；regress ALL PASS（regress-2180511821）；smoke1 15/15 + smoke2 8/8；probe 基线全符。
- **假红实录（PITFALLS #19/#33/#60 时序抖动类，非新坑）**：首跑 regress 双红（deep_test 内部 3 连败：attempt1 pinyin-settle 红连锁 25 FAIL、attempt2/3 仅 T10/T12 各 2 FAIL；e2e 三次 settle 失败点各异）。原样复跑即绿（deep_test 首试直绿；e2e 第 3 次过——**绿跑前两试仍 settle 红**，抖动未消失）。**纠偏：与"手清宿主"无因果**——regress.cmd 94/107 行本就先 taskkill 再跑各套件，两跑起点同为零宿主；非代码回归（改动仅菜单项+文件 IO 助手，TSF/管道/引擎路径零触碰）。教训：settle 类双红先原样复跑一次再排查，勿抢因果。

## 2026-10-31 · 三管理窗 Esc 统一 + 行解析 EOF 死循环修复

两线并一轮：交互收尾（Esc 通则）+ smoke3 暴露的真实挂死 bug 修复，引擎/公共 API 零改动。

- **三窗 Esc 统一**：blockedui（BlEscEditProc/BlEscListProc + g_oldBlEditProc/g_oldBlListProc）、phraseui（PhEsc*，两 EDIT 共享 g_oldPhEditProc）、hotphraseui（HpEsc*，**录制框保留 Esc=清空录制语义**——录制流中 Esc 另有含义不清空词框）——列表框 Esc=隐藏复用窗体（与 X 按钮同路），输入框焦点时 Esc 经子类化同样隐藏；三窗 WndProc WM_KEYDOWN Esc case 齐备。帮助文本（helpwnd.cpp kHelpText）词典与学习段 +3 行：自定义短语/快捷短语 Esc 说明 + "管理窗 Esc"通则。
- **挂死修复（PITFALLS #69，本批核心）**：smoke3 首跑实证——屏蔽窗首次"添加"后宿主 UI 线程 100% CPU 死循环（SendMessage 全无响应）。无 cdb 环境下自建取证链（tmp/threaddiag.cs：16 对齐 CONTEXT → Rip 采样 → dbghelp 跨进程符号化 → 项目帧过滤栈扫描）定位 blockedui.cpp LoadBlockedWords 行循环：EOF 虚拟空行命中 `continue` 旁路收尾 `break`，pos 停滞 → 死循环；SaveBlockedWords 尾 `\r\n` 使"首添加后 RefreshBlList 必挂"成为确定性路径。同型三处修齐（blockedui/phraseui/hotphraseui，统一"先记 last 再过滤"结构）；userdictwnd 与引擎解析器核查安全。
- **工装**：tmp/smoke1.ps1 热键环境护栏（RegisterHotKey 探测 1409 → 第 7 步降级 SKIP；本轮 StarMark 已退出故 15/15 真命中）；tmp/smoke3.ps1 屏蔽词走查 18 项新增（开窗/添加三端断言/重复添加/删除/清空确认/Esc 双路/复用/文件备份恢复）——dup 步须重填词框（首次成功添加即清框），首跑唯一红项即此脚本 bug；tmp/threaddiag.cs 线程取证器（`threaddiag.exe <pid> [tid] [samples]`，后续任何线程级取证可复用）。
- 门禁：build 零错；engine_test 220 / fix_readings / deep_test 34/34 / e2e exit=0；smoke1 15/15 + smoke2 8/8 + smoke3 18/18；probe 基线全符（gylg=4 候 / gylgl=供应链管理 1 候 / xian 首选西安）。
- 假红实录（PITFALLS #70）：regress 首跑 deep_test 25 FAIL + e2e 三连败——taskkill 后 DLL 重生宿主的 ~12s 初始化窗口被 deep_test 全程撞上（宿主就绪后单独复跑 34/34 直绿）；e2e 另被 Steam 弹窗/用户活跃终端抢前台与输入法状态竞争，最终 exit=0 收尾（pinyin mode settle 过）。

## 2026-10-31 · R7 频闪根治二 + 外观跟随系统 + 词框偶发不出现竞态修复

### 频闪根治二：ScrollWindowEx 像素级滚动（客观门禁全过，用户肉眼验收待定）

- **v1 双缓冲复盘**：只保护父窗自绘面；滚动时组合框/静态文本/开关等子控件在 MoveWindow(FALSE)+全窗失效后**各自独立重画、逐个跳现**——真频闪源。
- **v2 方案**：`ScrollSettingsTo` 统一滚动入口——`ScrollWindowEx(SW_SCROLLCHILDREN|SW_INVALIDATE)` 把现有画面（含子控件像素）BLT 平移、子窗同步位移、只失效新露窄条，**滚动期子控件零重画**；导航列与页脚带从 BLT 矩形排除（固定区），页脚单独父窗失效重画（无子控件参与不引频闪）。WM_VSCROLL/WM_MOUSEWHEEL 全走它。
- 门禁：build 零错；scrolltest v2 PASS（0→375→0，修脚本 wDown 编码 PITFALLS #74）；smoke1 15/15 + smoke2 8/8；probe 基线新鲜复验。

### 外观"跟随应用"改"跟随系统" + 设置页双主题（R7 任务①）

- **失效根因**：darkTheme 走 MsgHello 协议字段而 pipeclient.cpp:112 硬编码 0——"跟随应用"永远浅色（PITFALLS #72：跟随语义的信号源在链路上恒不可得）。
- **新文件 ui/uitheme.{h,cpp}**（挂 vcxproj，AGENTS 纪律 1）：SystemDarkMode（HKCU\...\Themes\Personalize!AppsUseLightTheme 直读，µs 级实时）/EffectiveDark(darkMode 0/1/2)/UiPalette 双主题调色板（浅=uistyle.h 令牌原值，深=Win11 同级灰阶暗变体）/UiSolidBrush 画刷缓存/UiApplyTitleBarDark（DWMWA_USE_IMMERSIVE_DARK_MODE(20)，不支持静默忽略）。
- **接线**：pipeserver 候选窗 `dark=EffectiveDark(st.darkMode)`；settingsui 全链路（PaintSettings 调色板化/WM_CTLCOLOR* 四路/DrawToggle/标题栏深浅/WM_SETTINGCHANGE("ImmersiveColorSet") 即时跟随）；文案四处同步（settingsui fillDark/helpwnd kDarkNames/settings.h 注释/README×2）。
- 门禁：build 零错；regress ALL PASS；smoke1 15/15 + smoke2 8/8。

### 词框偶发不出现：三处竞态修复（R7 任务③，e2e/smoke 待用户停机补跑）

- **A（主根因，PITFALLS #71）**：toast 1s 定时器到期瞬间用户开始打字——`Show()`（预测线程）清 _toastMode+KillTimer(1)，但 WM_TIMER 已投递 UI 线程队列、KillTimer 撤不掉；陈旧 WM_TIMER 随后派发把刚显示的候选完整隐藏（`if(_visible)` 恰被 Show 置 true）——"切换中英后整 1s 打字，头一两键无候选"机制。修复：WM_TIMER 加 _toastMode 守卫，已接管直接返回不清场。
- **B**：`ShowModeToast` 不置 `_visible=true`——纯 toast 到期不隐藏（卡空框）、`Hide()` 的 `!_visible` 守卫拦不住。修复：显示后置位。
- **C**：`PredictProc` 取 `_queue.back()` 后全局 `_queue.clear()`——7+ 进程各挂 DLL，切换应用瞬间旧应用尾巴帧吃掉新应用首帧。修复：只清同客户端条目，其余客户端帧留队列由后续轮次处理。
- 门禁：build 零错（注意 PITFALLS #75：宿主运行中构建照常成功但运行宿主仍旧二进制）；e2e/smoke/宿主热替换待用户停机后统一补跑，与频闪/主题批一并提交。

### 工装与事故

- **newtask-verify.ps1 v2**：SendMessageW 增加 string/StringBuilder 重载（v1 裸 string→IntPtr 编组静默失败）；WM_COMMAND wparam 字序修正（LOWORD=控件ID、HIWORD=通知码）；**删除围栏**——搜索框过滤隔离测试词条，匹配数≠1 一律拒删；导航坐标 GetDpiForWindow 换算（PITFALLS #73）。
- **用户词典误删事故（v1 脚本）已向用户披露**：514→513（文件序末位真实词条，三重工装缺陷叠加所致，不可恢复），引擎自学习可重建。

## 2026-10-31 · 30+ 功能自主开发批（用户授权"一直开发"，仅后台门禁）

用户授权持续自主开发 30+ 新功能且不占用前台——所有验证只走后台门禁（build 零错/engine_test 沙箱/fix_readings），e2e/smoke/真机走查押后至用户空闲统一补跑。先提交 R7 批建干净基线（`ef67921`），再按"后台可验性"排序开发。

### 批次 H：引擎便民格式批（`1e57398`）

- **rq 大写日期**：CnUppercaseDate（二〇二六年十月三十一日——年逐位读、月日口语读法"十月三十一日"）+ 无补零格式（%u年%u月%u日）。
- **组合格式**：rqsj 新增"日期 星期X"；sj 新增中文时分（14时30分）+ 12 小时中文（下午2时30分）；xq 新增"礼拜X"。
- **emoji 融合表 +15 社交高频词**：收到👌/好的👍/再见👋/抱歉😞/没关系🤗/开心😊/难过😔/无聊🥱/睡觉😴/吃饭🍚/庆祝🎊/成功✅/失败❌/注意❗ 等。
- **engine_test P11 扩展断言块**：rq 含〇/sj 含时/xq 含礼拜/rqsj 含星期（Report 数 220→224）。
- 门禁：build 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。

### 批次 B：数据每日自动备份（`d919215` 内，与批次 A 合并提交）

- **新文件 ui/autobackup.{h,cpp}**（自包含模块，usagestats 模式：自己的文件 IO、不依赖 uiinternal.h）：`AutoBackupTick`（ApplyLearn 选词公共落点触发，当日短路 + 跨重启幂等查当日目录存在）/`AutoBackupNow`/`BackupsDirW`；备份五文件（userdict/blocked/phrases/hotphrases/usage.txt，存在才拷）到 `%APPDATA%\LingxiIme\backups\daily-YYYYMMDD\`；**轮换保留 14 份**（daily-* 字典序=时间序，RemoveDirRecursive 删最旧）；沙箱（LINGXI_SETTINGS_NS 非空）自动停用。
- **托盘菜单**：2019"立即备份全部数据"（MessageBox 反馈份数）+ 2020"打开备份文件夹"（ShellExecuteW）。
- 设计动机：词典误删事故（512→513）暴露用户数据零兜底——每日首次打字自动全量快照，误操作/文件损坏可回溯 14 天。

### 批次 A：打字统计增强 + 每日目标提醒（`d919215`）

- **settings**：新增 `dailyCharGoal`（默认 500 字，0=关闭；Load/Save/SaveDiff 三处）。
- **usagestats 扩展**：UsageData 新增 peakDayChars/peakDayKey/goalReachedDay（解析 peakChars=/peakDay=/goalDay= 键值行）；StatsAddPick 实时刷新单日峰值；`StatsMarkGoalReached(dayKey)` 防当日重复提醒；BuildStatsText 增"历史最高单日"行 + "【今日目标】X/Y 字（Z%）✓已达成"进度行（读 goal 设置）。
- **候选窗通用文字气泡 `ShowTextToast`**（window.h 声明 + candwnd_core.cpp 实现）：复用 toast 态机（WM_TIMER(1) 到期隐藏 / Show/SetPage/ShowModeToast 候选接管清态 / _visible 置位），宽度随文字自适应（_toastFont 实测 +28px padding），驻留 1.6s；**五处清 _toastText 防残留**（Show/SetPage/ShowModeToast/WM_TIMER 到期/OnPaint 读取）；OnPaint toast 分支渲染 _toastText（非空画文字深浅适配，空画中/英原样）。
- **ApplyLearn 目标达成检测**（pipeserver.cpp，锁外）：dailyCharGoal>0 && dayChars>=goal && goalReachedDay!=dayKey → StatsMarkGoalReached + `ShowTextToast(L"今日目标达成")` + 日志；沙箱宿主统计全零天然不触发。
- **统计窗 CSV 导出**（statswnd.cpp，IDC_STATS_CSV=363"导出 CSV..."）：GetSaveFileNameW 另存对话框；指标/数值两列 11 行（日期/今日与累计选词与汉字/活跃/连续/峰值/目标）；UTF-8 带 BOM（Excel 直开中文不乱码）+ \r\n 行尾；窗宽 460→560 容纳第四按钮。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。
- 工装坑固化 PITFALLS #76：PowerShell 嵌套 `\"` 先被 PS 剥一层、MSBuild（.NET 规则）把 `\"` 当引号转义吞掉 `/v:m /nologo`——git 长 message 改 `-F 文件`、MSBuild 尾反斜杠属性写 `\\`；engine_test 末行"=== 结果: PASS=.. ==="在重定向下被 wcstombs C locale 转换截断只剩"=== "——门禁计数改 `Select-String '\[PASS\]'` 统计。

### 深度审计：批次 A/B 数据完整性缺陷修复（`0677401`）

用户指令"继续深入分析挖掘潜在问题"——对已提交批次做代码级审计（读实现全文、列缺陷清单、逐项核验），确认 5 项真实缺陷修复、4 项疑点排除：

- **usage.txt 非原子写（高）**：WriteTextFile CREATE_ALWAYS 直写有两个撕裂窗口——进程中途被杀留半截文件（下次启动解析出部分值）；写入期间备份 CopyFileW/诊断导出恰好读到空/半截文件。改 **.tmp+MoveFileExW REPLACE_EXISTING**（与引擎字典文件同模式），失败删临时文件。
- **备份跨重启幂等认半截（高）**：原认账依据"当日目录存在"——进程在 BackupInto 中途被杀（拷到 2/5 文件）后新实例按目录认账，当日备份永久残缺。改 **done.flag 完成标记**：BackupInto 成功才写；幂等检查查标记而非目录；无标记（半截）重跑覆盖式补拷全量。
- **备份失败重试风暴（中）**：BackupInto 失败不置 g_doneDay → 个别文件被占用时**每次选词**重跑 5×GetFileAttributes+5×CopyFileW+轮换枚举。改**成败都认账当日**+LX_LOG 告警；重启后因标记缺失自动补试（原子写落地后 CopyFileW 撞 SaveLocked 写入窗口这一失败源已消除）。
- **午夜取时撕裂（低）**：TodaySuffix/TodayDayKey 两次独立 GetLocalTime 在 00:00 边界错开 → "认账日期"与"备份目录"差一天、午夜后首日漏备份。合并 **TodayBoth 单次取时**。
- **ApplyLearn 每选词全量读注册表（中）**：LoadSettings 约 30 项 RegGetValueW 跑在选词热路径。改 **16 次选词刷新一次的 std::atomic 缓存**（注册表 IO 降 94%；达成检测幂等，多客户端线程读到新旧值均无害）。
- **疑点排除**：tray 2020 打开备份文件夹已先 CreateDirectoryW（无缺陷）；RotateOldBackups 已校验 FILE_ATTRIBUTE_DIRECTORY；StatsMarkGoalReached 落盘失败时内存 goalReachedDay 已置位不会重复弹 toast；BackupInto"全源缺失"路径（全新安装首选拷空）由"成败都认账"覆盖，次日全量补齐。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。

### 批次 C：剪贴板历史窗（`66bb3d0`）

- **新自包含模块 ui/clipboardui.{h,cpp}**（autobackup/usagestats 模式：自己的文件 IO 与监听窗，不依赖 uiinternal.h）：隐藏监听窗（**普通隐藏 POPUP 而非 HWND_MESSAGE 消息专用窗**——WM_CLIPBOARDUPDATE 虽是直投而非广播，消息专用窗场景文档未承诺）+ AddClipboardFormatListener，全局复制文本自动入史。
- **捕获口径**：纯空白跳过；单条 ≤2000 字符截断；与最近一条相同跳过（历史窗复制回声去重）；已存在前移不重记；上限 50 条。
- **存储 clipboard.dat**：magic LXCB + 版本 + 条数 + 记录 [u32 字节长][i64 本地时间][UTF-8]；加载全程边界校验（截断文件保留已读部分）；保存 .tmp+MoveFileExW 原子替换（0677401 审计同模式）。
- **节流**：捕获每 5 次落盘 + ClipboardFlush（WM_ENDSESSION/WM_DESTROY）补写；删除/清空立即落盘。
- **历史窗**（blockedui 模板）：列表（"MM-DD HH:MM 预览"，换行折叠空格、64 字符省略号）+ 只读全文预览框（选中实时刷新，空态/未选态文案）+ 复制选中/删除选中/清空/关闭；**双击条目直接复制回剪贴板**；Esc 三路关窗（窗体/列表/预览子类化）。
- **接线**：托盘 2021"剪贴板历史...（Ctrl+Alt+V）"；全局热键 Ctrl+Alt+V（kTrayHotkeyClip=5，占用容错）；kHelpText 全量同步；沙箱停用监听与窗口。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。

### 批次 D：管理窗体验 + 托盘版本号 + 诊断数据全景（`ba58c1f`）

- **管理窗标题条目数**：屏蔽词/自定义短语/快捷短语三窗标题实时携带条数（Refresh 时 SetWindowTextW——任务栏/窗口列表一眼可辨规模）。
- **列表双击回填**：三管理窗双击列表条目回填输入框（屏蔽词→词框；自定义短语→缩写+短语；快捷短语→**录制框捕获态 g_capMods/g_capVk 同步置位**+句式框），编辑后重新添加。
- **托盘版本号项**：菜单底部灰显"版本 1.0（构建于 __DATE__）"（与 --version 同源；TPM_RETURNCMD 对灰显返回 0 无副作用）。
- **--diagnostics 数据全景**：新增"每日目标：N 字/日　屏蔽词：N 条　自定义短语：N 条"与"每日备份：N 份（保留 14）　剪贴板历史：N 条（上限 50）"两行；新公共 API autobackup `DailyBackupCount()`（只读枚举 daily-* 目录）+ clipboardui `ClipboardHistoryCount()`；**cmd /c 包裹重定向实测**两行精确落盘（PS 不等 GUI 子系统进程，#R6 坑重申）。
- **kHelpText 同步**：管理窗列表双击回填速查行。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0 + --diagnostics 实证，全后台。




### 批次 E：符号面板收藏页（e39107d）

- **收藏页（第二标签，动态分类）**：右键网格符号=收藏/取消收藏（日期页条目随开随算除外）；已收藏符号格右上角小★角标（12px 小字体，皮肤强调色）；上限 60 条、最新在前；右键"收藏"标签=确认后清空（收藏为用户主动沉淀，较"最近"页多一道确认）。
- **标签序调整**：0=最近 / **1=收藏** / 2..15=静态分类 / 16=日期；面板宽随 17 标签增至 846px。注册表 **lastSymbolCat 语义不变**（仍存静态分类 1 基序），加载 +1 / 销毁 -1 两侧换算，老版本写入值免迁移。
- **持久化**：settings 新增 favoriteSymbols（\x1F 分隔，仿 recentSymbols 三处接线：Load/Save/SaveDiff）；改动即落盘（右键切换/清空），注册表随行导出无需备份改动。
- **收藏页高度随动**：增删条目行数变时 SetWindowPos+圆角重设（与切页同款）；空态提示"暂无收藏（右键任意符号收藏，最多 60 个）"。
- **搜索框押后**：NOACTIVATE 面板加编辑框须 SetForegroundWindow 抢焦点交互（TSF 宿主焦点路由敏感），归入前台验证批次统一做。
- kHelpText 同步（右键符号/收藏页/关闭路径三处）。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。

### 批次 F：emoji 分类高频扩充（e0443ff）

- **5 分类扩容**：表情 35→48（😅😆🤗😙🥺😬😰😖😪😌😛😝🤠）/ 手势 25→36（书写/身体语言组：✍️🤌🤏💅🙆🙅🙇🙋👂👀👣）/ 动物 34→48（走兽/家禽/神话组：🐺🦌🐎🐑🦢🕊️🐇🐈🐕🦔🐾🐉🐲🦚）/ 食物 36→48（主食/甜点/饮品组：🍚🥘🍲🥟🍤🍥🥠🍦🍰🎂🥛☕）/ 活动 30→42（出行/地标组：✈️🚗🚌🚲🚄🚢⛵🚀🏝️🏖️⛰️🌋）。
- **兼容性约束**：全部单码点或 +VS16，避开 E13.1+/ZWJ 序列防 Win10 渲染缺失；新增条目 \u 转义防 VS16 不可见字符在编辑中丢失。
- 门禁：build Debug 零错 + engine_test 沙箱 **224/0** + fix_readings 缺口 0，全后台。

### 批次 C-F 真机验证收口（smoke4 12/12）

- **smoke4.ps1 12 项全过**：剪贴板 5 连拷节流落盘（clipboard.dat 前缀扫描）/ 托盘消息窗 / WM_HOTKEY 5 开剪贴板历史窗 + WM_CLOSE 隐藏语义（关=藏，保控件状态）/ 诊断全景两行 / `--symbols` 第二实例转发开面板（命名事件→NotifyThread→Post msg 实证）/ 17 标签宽 846 / 右键收藏↔取消收藏（注册表回读对称）/ **表情页 48 条 4 行高 186（批次 F 直接验证）** / LastSymbolCat 恢复。
- **首跑 5/7 两 FAIL 系探针假阴性，非代码缺陷**：$null→"" 编组（PITFALLS #77）使 `FindWindowW(cls, $null)` 只匹配空标题窗口——运行期创建的三个标题非空窗口（帮助/剪贴板/面板）全体假阴性；diag7-11 取证链（EnumWindows 码位 dump / FW(cls+title) 对照 / [NullString]::Value 复验）实锤；修正探针后 12/12。宿主全链路（WM_HOTKEY 3 暂停翻转注册表+日志、4 帮助窗、5 剪贴板窗、转发事件）零缺陷，"僵尸宿主"为工装误诊（#78/#79 叠加）。
- **smoke4 工装修正**：CbWnd 断言改"存在且已隐藏"；测试前后保护/恢复用户剪贴板。

### 批次 G：无痕模式（7d6b0a）

- **语义**：临时不留痕迹——选词不学习/不计统计/不触发每日备份与目标提醒、不记最近符号、不录剪贴板历史；预测与收藏（显式动作）不受影响。托盘一键切换，下一选词即生效。
- **Settings.incognito**：注册表 Incognito DWORD，LoadSettings/SaveSettings/SaveDiff 三处接线；common/settings 增单键快读 IncognitoActive()（RegGetValue µs 级，沙箱感知 LINGXI_SETTINGS_NS），热路径零缓存保开关精确性。
- **门禁接线**：pipeserver ApplyLearn 顶部早退（学习/统计/备份触发/目标提醒四合一）；symbolpanel AddRecent 不记最近；clipboardui CbListenProc WM_CLIPBOARDUPDATE 不录历史。
- **托盘**：菜单 2022"无痕模式（临时不留痕）"勾选切换 + BuildTrayTip"· 无痕"标注 + kHelpText 两处同步（托盘菜单列表/词典与学习段）。
- **engine_test P8c 断言块**（Report 224→228）：IncognitoActive 缺键 false/置位 true/LoadSettings 往返一致/清除 false。
- 门禁：build Debug 零错 + engine_test 沙箱 224/0 + fix_readings 缺口 0，全后台。

### 批次 H：帮助窗 Ctrl+F 查找 + 诊断无痕行（d125be8）

- **查找行**（底部按钮条上方，默认隐藏）：Ctrl+F 呼出（已开则回查找框全选备改）/Esc 关查找行不关窗；F3 无修饰键直达（未开则先开，Windows 惯例）。
- **查找行为**：输入即跳转（EN_UPDATE 从当前选中首起）；Enter/"下一个"按钮/F3=从选中末尾续找；尾部未中回绕开头；仍未中按钮反馈"未找到"（下次输入自动复位）；查找框预填当前选中片段；大小写不敏感（towlower）。
- **布局收口**：WM_SIZE 体改调 HelpLayout()（正文/按钮条/查找行三段统一，拉伸不漂移）；查找三控件创建挂 ShowHelpWindow 建窗段（子类化 HelpFindEditProc：Esc/Enter/Ctrl+F 三键）；WM_DESTROY 状态复位（g_findVisible + 句柄组），重开默认隐藏。
- **诊断无痕行**：--diagnostics 设置行补"无痕=%u"（smoke5 断言点）；kHelpText 同步（Enter/F3=下一个）。
- 门禁：build Debug 零错 + engine_test 沙箱 224/0 + fix_readings 缺口 0 + --diagnostics 实测"无痕=0"（exit 0），全后台。
- **smoke5.ps1 预置**（G+H 真机验证脚本，用户空闲+宿主重启后跑）：G1-G5 诊断翻转/无痕期 clipboard.dat 不增长且不含 marker/关无痕恢复记录；H1-H12 帮助查找全链——F3 无修饰键可 PostMessage 直达子类过程（绕开 Ctrl+F 的 GetKeyState 异步键态限制），SetWindowTextW 触发 EN_UPDATE 实测输入即跳转、EM_GETSEL 断言选区推进、"未找到"反馈与复位、Esc 只关查找行、重开状态复位。全文用户状态保护/恢复（Incognito/剪贴板）。
### 深度审计轮 2（C-H 全面走查，零缺陷结论 + smoke5 断言自纠）

- **审计范围与结论**（逐文件深读，无 actionable 缺陷）：clipboardui（LXCB 加载边界校验/原子写/回声去重与"已存在前移"语义一致/ClipboardFlush 双路接线 WM_ENDSESSION+退出前）、批次 D 三窗双击回填（hotphraseui g_capMods/g_capVk 捕获态同步正确、blockedui/phraseui 同模式）、statswnd CSV 导出（BOM/取消不写/wstring 临时串生命周期安全）、symbolpanel 收藏页（\x1F 解析端界/60 上限/差异持久化）、tray 菜单与定时器资源配对（CreatePopupMenu↔DestroyMenu、SetTimer↔KillTimer 全对称）、uitheme（画刷缓存 24 槽满兜底 stock brush/DWM 属性动态解析失败静默）。
- **smoke5 断言自纠**：查找针"输入"kHelpText 全文仅 2 处（标题+"一键输入句式"），H7 第三次续找回绕到首个匹配致 u0>t0 假失败——改"候选"（≥8 处）；F3 无修饰键可 PostMessage 直达子类过程（绕开 Ctrl+F 的 GetKeyState 异步键态限制）是 smoke5 可全自动的关键设计。
- **门禁**：Release 构建补跑绿（自 P0 后多批未跑）；probe 基线复验全符（新二进制 gylg=4 候/gylgl=供应链管理/xian→西安/xyzxyz 空/长句尾 23≥20）——批次 G/H 零引擎回归。
- **build32 LNK1168**：Debug32\ImeCore.dll 被运行中 IME 进程加载，用户使用期不可解锁——记入空闲收口 runbook（tmp/runbook_idle.md：重启宿主后补跑或注销重登再跑）。
### 深度审计轮 3（滚动 v2 / pipeserver 主链 / usagestats 解析，零缺陷结论）

- **settingsui 频闪 v2 滚动**：ScrollSettingsTo 滚动矢量方向/导航列与页脚带 BLT 排除/SW_SCROLLCHILDREN 幂等重排/空矩形守卫全正确；防抖循环信号量幻影信用由 have=false 空转轮兜底，无饥饿无丢帧。
- **pipeserver 主链**：EnqueuePredict 每客户端只留最新+全局有界；PredictProc 按客户端裁剪（任务③修复位注释详尽、实现正确——全局 clear 早已退役）；空文本隐藏/SEH 熔断/暂停与补全门禁/cover-cands 平行校验全对。
- **usagestats 解析**：键名带 '=' 比较防前缀碰撞（"day=" 不会误吞 "dayPicks="）；_strtoui64 垃圾容错；BOM 跳过/CRLF 修剪；全键解析与 SaveLocked 写出全对称。
- **regress 兼容确认**：regress.cmd 判据=engine_test exit code 而非断言数——P8c（224→228）不破坏收口；build_et.cmd 与 regress 内 engine_test 构建同为 Release 配置，P8c 已在收口同构环境验证 228/0。
- 结论：今晚三批审计（轮 2/轮 3）覆盖 C-H 全部新代码与 G/H 全部门禁接线点，**零 actionable 缺陷**；唯一修正为 smoke5 断言针自纠（工具侧，非产品代码）。
### 深度审计轮 4（候选窗 toast 态机，零缺陷结论）

- **candwnd toast 态机**（批次 A 引入、任务③加固，最常用 UI 面）：五处清 _toastText（Show/SetPage/ShowModeToast/WM_TIMER 到期/render 读取快照）逐点核对全对称；ShowTextToast 自适应宽度（GetTextExtent+28px padding）/1.6s 驻留/SWP_NOACTIVATE 无抢焦/圆角重设全对；已投递 WM_TIMER 竞态由 wasToast 双分支守卫（真 toast 到期走隐藏/候选接管直接返回）——PITFALLS #71 修复位实现与注释一致。
- 微瑕不修：ShowModeToast ool dark=_dark; ... _dark=dark; 自赋值死代码（无行为影响，不值得 churn）；ShowTextToast 在首帧渲染前调用时 _toastFont 为 NULL——SelectObject(NULL) 失败落默认字体量宽，实际调用链（ApplyLearn 目标达成）必在首次渲染后，不可达。
- **今晚审计总结（轮 2-4）**：clipboardui/批次 D 回填/statswnd CSV/symbolpanel 收藏/tray 资源/uitheme/settingsui 滚动 v2/pipeserver 主链/usagestats 解析/toast 态机——**十模块零 actionable 缺陷**；工具侧修正 1 处（smoke5 断言针"输入"→"候选"）。
## 2026-10-31 · 批次 I emoji 融合表问候/节日/生活扩充（6d97f60，全后台）

- **EmojiFusionMap 48→63 词条**：早上好🌞/早安🌅/晚上好🌃/晚安🌙/新年快乐🎆/春节🏮/中秋🥮/端午🥟/圣诞🎄/干杯🍻/红包🧧/亲亲😘/抱抱🤗/可爱🥰/我爱你❤️。
- **兼容档纪律**：Unicode 6.0/E11 上界（E12+ 如🧋气泡茶不入，防 Win10 渲染缺字——批次 F 规则沿用）；VS16 对显式 \u2764\uFE0F 转义防源文件丢不可见字符；触发词 15/15 词典逐一验证（"爱你"不在词典→改用"我爱你"）。
- **engine_test P13 扩展**：5 代表断言（wanan/chunjie/hongbao/woaini/zaoshanghao，含 VS16 对等值比较），Report **228→233**。
- 门禁：build Debug 零错 + engine_test 沙箱 **233/0** + fix_readings 缺口 0，全后台零前台依赖。
- 审计轮 4 收口见上节；runbook 回归预期同步 233，肉眼清单补 emoji 抽查项。
### 深度审计轮 5（引擎核心 pinyin.cpp 2119 行，1 真缺陷已修）

- **真缺陷（已修 efc7490）**：ComputeFuzzy 声母 else-if 链——"l" 同挂 bit3（l→n，默认开）与 bit6（l→r）两支，双组同开时 l→r 分支永不可达（r→l 正常，单向不对称）；掩码变更经 BuildFuzzyIndex 直达预计算表。修法=else-if 链改独立 if（各条件 initial 判等自守卫，仅 "l" 可命中两支，其余行为等价）。P8b +1 回归断言（0xFF 双组 lang 出"让"），Report 233→**234**。
- **逐块核对干净**：Predict 尾段（ups 双向去重/两向量同长 resize/繁体注入后统一转换，日期字与 emoji 不受繁转影响）；InjectExplicitTop（covers 同步/截断同步）；Learn（双拼键同构转换/每键 8 条淘汰最小计数/锁内含索引更新、KickPersist 锁外）；Generate 全段（tier 基准/匹配缓存 span≤6<7/B3 rawLen 回放/长纯声母近全跨度受限发射/质量三闸（词长偏好 1.5、低频变体 +2.5、纯简拼整词限制）/残段 headOk 门/稳定排序确定性）；LoadUserDict（三段制表符解析全验证 word/key/count>0）；**KickPersist 竞态矩阵三交错全覆盖**（CAS 成功直启/失败由收尾重查兜底/写失败恢复脏位重试）+ WriteFileAtomicA 原子写 + detach 线程进程退出安全（全局单例无运行期析构）。
- **微瑕不修（记录）**：①520-521 悬挂 if（key.size()>=2 守卫体即第二条 if，语义正确但两行间插语句即碎，风格脆弱）；②521 NormKey(raw) 冗余重算（=511 行 key，微秒级）；③手编 userdict.txt 键未规范化容忍缺口（"ni hao" 带空格键可加载但永不匹配 Predict 查键——内部格式自有写方恒规范化，非功能缺陷）。
- 门禁：build Debug 零错 + engine_test 沙箱 **234/0** + fix_readings 缺口 0，全后台。
### 深度审计轮 6（llm.cpp 续写 KV 状态机 2 真缺陷已修 + ngram/main 干净）

- **llm.cpp 续写 Generate 两缺陷（已修 06447e）**：A）decode 失败路径 ResetKv 清空 KV 后，循环外无条件 _genKv.hasPrev=true 把增量态覆盖回来——下次入口对空 KV 的 seq_rm(prompt.size()) 空范围空转"成功"（llama.h:760 仅"部分删除失败"才 false），公共前缀照常匹配，delta tokens 从位置 0 错位放置→静默垃圾续写；B）get_logits_ith 空中断只置 hasPrev=false 不清 KV——残留 prompt+半程生成尾，下次 common=0 全量 decode 挂在陈旧 KV 末尾错位（pos=null=追加语义，代码单 token 步进自身依赖此语义），保存后自我延续直到 KV 溢出才 ResetKv 又进 A 链。修复=kvClean 标记三处（decode 失败/logits 空→ResetKv+false；保存门 
>0 && kvClean；多路采样门加 kvClean）。
- **对照重排槽（_rrCtx）同构路径逐行核对干净**：prefill 失败在 746 保存点前早退；生成中断残留恰为"prompt+生成尾"标准形；Borda 短路守卫（merged.empty() 先判）/sampler chain 所有权/EMA 吞吐降级/重罚路径全对——同型缺陷重排侧不存在，侧证修复点定位精确。
- **ngram.cpp（390 行）全文干净**：StripContinuationEcho 下溢边界逐一验（k≥2 守卫下 maxK<2 零迭代不落 k--）；DedupCompletions ②前缀堆叠双分支（n 延伸 kn/kn 延伸 n）两方向对称；ChainPredictor midWord/pureAscii 路由与 B4 ASCII 过滤兜底全对。微瑕：注释"前 5"对 kMaxCands=9 漂移。
- **main.cpp（595 行）全文干净**：装配序安全（托盘创建先于 g_hooks 赋值，但菜单展示必在消息泵启动后=赋值已完成）；chain 持有 sharedLlm 所有权、delete engine 单点释放；六处 CLI 早退 LocalFree 全对称；singleton Mutex/hReady/NotifyThreadParam static 生命周期全部正确；g_hotPhrases 仅 UI 线程读写（注释声明与实际调用点核对一致）。
- 门禁：build Debug 零错 + engine_test 沙箱 **234/0**（LLM 无模型沙箱不可达=编译级回归）+ fix_readings 缺口 0，全后台。

### 深度审计轮 7（备份/导出静默失败 2 处真缺陷已修 + tray/userdictwnd 干净）

- **settingsbackup.cpp 真缺陷（已修，268ee54）**：ExportSettingsBackup 随行文件（用户词典/热键短语/屏蔽词）读成功但写失败仅记日志，弹窗仍报"已导出"——虚假信心（备份不完整用户不知）；ImportSettingsBackup 用户已确认恢复但写失败同静默（词典/短语/屏蔽词未被覆盖却报"已导入"）。修复=sidecarFail/restoreFail 计数，任一>0 弹 WARNING 明示"备份不完整/当前文件未被覆盖"，日志加 fail 字段；源文件不存在=正常（新装）不告警。
- **userdictwnd.cpp 真缺陷（已修，268ee54）**：UdExport 读源失败或写目标失败完全静默——与 settingsbackup 同根因（用户误以为已备份）。修复=失败弹 WARNING"未生成备份文件"。其余 753 行全文干净：删除/升降频按内容匹配+索引回退+落空刷新（引擎并发重写防漂移）；WriteFileBytes tmp+MoveFileEx 原子写与引擎双写者共存；右键词典操作全路径（删全读音条目/Pin +10000/降频钳位 1/新增键推导全拼守卫/写失败 false+日志）；UdAddWord 键归一化与引擎 NormKey 口径一致。微瑕不修：UdClearAll 忽略写结果（安全方向，RefreshUdList 让用户看到真相）、删除路径恒真 if(removed)、Nudge 漂移早退未刷新列表。
- **tray.cpp（422 行）全文干净**：托盘资源配对（NIM_ADD/NIM_DELETE 对称）、定时器三清点、热键三注销、ENDSESSION 双 flush、菜单命令全分发核对。微瑕不修：60 分钟到期日志文案写"30 分钟"（仅日志文案，菜单/弹窗文案正确）、卸载脚本路径限开发目录。
- 门禁：build Debug 零错 + engine_test **234/0** + fix_readings 缺口 0，全后台（regress 含 taskkill 宿主项押后空闲收口）。

### 深度审计轮 8（候选窗三件套 + pipeserver + statswnd 全文零缺陷）

- **candwnd_render.cpp（413 行）全文干净**：三字体重建先删后建防 GDI 泄漏；皮肤越界回退默认；tooltip 懒建+常驻复用+工作区钳制；OnPaint 全路径快照锁内拷贝/画刷全删/组合串掐头保尾循环 size_t keep>=1 无下溢；NULL_PEN+RoundRect 选中态；构建戳 LINGXI_BUILD_STAMP 门控。存疑点排除：LayoutMetrics 直读 candidateFontSize 无钳位——LoadSettings 侧已消毒（settings.cpp:89 仅接受 12..32 否则默认 16）。
- **candwnd_interact.cpp（777 行）全文干净**：剪贴板 3×30ms 重试+SetClipboardData 成功让渡所有权失败自清；W2M+RFC3986 百分号编码联网搜索；拖拽工作区钳制+toast 守卫（防 toast 小窗坐标写进固定设置）；滚轮三手势（Shift 不透明度 70-100/Ctrl 字号 12-32/裸滚轮翻页 long long 夹取）；右键词典菜单全路径含不抢前台根因注释（旧实现 SetForegroundWindow 致 TSF 焦点离开宿主→SendHide）；锁内快照锁外回调（_pick/_btnCmd 不持 _cs 调用）。
- **pipeserver.cpp（745 行）全文干净**：SEH 三外壳 C2712 合规（POD 参数传递）；单实例 FILE_FLAG_FIRST_PIPE_INSTANCE 冲突 ExitProcess；客户端 shared_ptr 引用计数管句柄生命周期（断线不死锁不悬垂）；CTX 双格式 V2/V1 长度自洽判别、损坏帧丢弃；有界队列 8+每客户端去重+防抖合并（多客户端下被弃应用帧丢弃属良性设计——该应用必已 Deactivate）；预测期零持锁；两阶段重排 gen+rrSeq 双代序闸+400ms 观察窗+900ms 预算+RerankStillCurrent 二次核对；cover 与 cands 平行校验三处一致；B1 学习键=消耗前缀。微瑕不修：Stop() 不 join 分离的重排线程（仅退出路径，进程拆除兜底）。
- **statswnd.cpp（297 行）全文干净**：CSV/诊断报告导出按结果弹窗（与轮 7 静默失败修复同口径，本文件本就正确）；清零确认对话框；Esc 双路（正文子类化+窗口本体）；单例 IsWindow 复用；子类化指针复建正确（SetWindowLongPtrW 返回原始 EDIT proc）。
- 结论：至此 ui 层全部窗口/交互/管线文件完成深度审计（轮 2-8 累计 17 文件，真缺陷 6 处全部已修）。门禁：build Debug 零错 + engine_test **234/0** + fix_readings 缺口 0，全后台。

### 深度审计轮 9（common 层五小件 + candwnd_core + autobackup，7 文件零缺陷）

- **pipe_util.cpp（210 行）全文干净**：管道 DACL 限定当前用户 SID（SDDL D:(A;;GA;;SID)）失败回退默认 DACL；ERROR_PIPE_BUSY 500ms 重试；帧累加器读法（want+64 超读下一帧/total 上限 1MB 拒非法帧/多余字节保留/transferred==0 判管道关）；WriteFrame 超时取消+transferred 全量校验。微瑕不修：GetUserSA 的 s_init 非原子（实际调用拓扑单线程，至多重复分配一次）。
- **proto.h（159 行）干净**：pack(1) 结构体与 pipeserver 双格式解析端逐字段自洽（MsgCtx=32B/V1=28B，长度算式两侧一致）；AppendStr u16 钳 0xFFFF；ReadStr/ReadPod 越界全检；NextPageStart 夹取纯函数。
- **settings.cpp（255 行）全文干净**：沙盒键名白名单 [A-Za-z0-9_-] 严检（非法宁可不隔离）；33 键加载逐键范围消毒（越界保默认）；ToggleMode/ShiftToggle 双键兼容派生对称写；SaveSettingsDiff 单键 diff 防"整页回写"覆盖未同步状态；GetString 双读尺寸法+2KB 上限+NUL 截断；负坐标 DWORD 往返正确。
- **log.h（127 行）干净**：LX_LOGV 宏 if 短路（关闭时参数表达式不求值）；常量初始化门控避开 magic static Debug 检查；文件日志 CS 串行+1MB 轮转 .old 留档+UTF-16 原生写（读取须 -Encoding Unicode，PITFALLS 口径一致）。
- **punct.h/settings.h 干净**：标点映射纯函数 out[3] 界内（最长 2 字符+翻号状态翻转）；Settings 默认值全部落在 LoadSettings 消毒区间内（16∈[12,32]/0xFF/1≤4/100∈[70,100]…逐项核过）。
- **candwnd_core.cpp（489 行）全文干净**：Shutdown 刻意不 DeleteCriticalSection 的退出竞态论证（存活线程踩已删 CS，泄漏无害）；LayoutAndPosition 锚定三级回退链（caret→鼠标位→记忆位）+工作区双轴钳制+同步补画防残影；Show/SetPage 的 ver 保页语义与 DLL 侧 rerank 判定对称；WM_TIMER 陈旧定时器 wasToast 守卫；CurrentPageRangeLocked 规整边界（start>total 回 0/start≥total 末整页）。轮 4 微瑕记录复核：ShowModeToast 自赋值死代码仍在（不修维持原判）。
- **autobackup.cpp（208 行）全文干净**：TodayBoth 单次 GetLocalTime 防午夜撕裂（suffix/dayKey 同源）；done.flag 幂等认账（半截备份无标记→重启补拷，目录存在不认账——注释里记录的缺陷修复实现正确）；成败都记 g_doneDay 防选词热路径文件系统风暴；轮换字典序=时间序保 14 份；g_mtx 串行打字路径与手动路径；沙箱探测单次。
- 结论：common 层（IPC/协议/设置/日志/标点）+ 候选窗核心生命周期 + 每日备份全部干净，真缺陷 0。门禁：build Debug 零错 + engine_test **234/0** + fix_readings 缺口 0，全后台。

### 深度审计轮 10（helpwnd + symbolpanel + 四管理窗 + settingsui 全文，2 真缺陷已修）

- **helpwnd.cpp 真缺陷（已修）**：WM_MOUSEWHEEL 不检查 Ctrl 直接调字号增减——帮助文本与注释均写明"Ctrl+滚轮改字号"，焦点在按钮等处时裸滚轮误改字号（焦点在正文时滚轮被子控件吃掉才长期掩盖）。修复=补 Ctrl 检查，非 Ctrl 滚轮转发正文滚动（焦点在按钮等处时也能滚文本，UX 增益）；smoke5 核对无滚轮/字号断言，修复安全。其余干净：BuildHelpText 数组索引全消毒（分类/条目/命令三表边界逐一验）、查找回绕、复制 3×30ms 重试、GWLP_USERDATA 存正文 EDIT 的子类化链。
- **settingsui.cpp（1245 行）全文（轮 3 已审滚动段）1 真缺陷（已修）**：WM_MOUSELEAVE 的 g_trackingMouse=false 只在 g_navHover!=-1 分支内执行——鼠标从内容区直接移出窗口时 hover 态为 -1，标志滞留 true，TrackMouseEvent 不再重挂；此后悬停导航再离窗收不到 LEAVE，**hover 药丸滞留绘制**。修复=无条件重置（TME_LEAVE 本就一次性，离开即失效须重挂）。其余干净：owner-draw 勾选态丢失真凶注释与外置 g_toggleOn 全链一致；创建期 g_updating+g_controlsLoaded 双保险防刷零（EN_CHANGE/BN_CLICKED 全挡）；fuzzyGroups 8 bit 装载/保存双侧对称；SaveFromControls 全控件钳位（count 1-9/toggleMode 0-4/skin 0-6/uiFont 0-3/字号表驱动）；窗口位置恢复监视器交集校验；SB_BOTTOM 用 nMax-nPage。微瑕不修：101 行注释"bit0..bit4"对 8 组漂移；句柄失效重建路径旧字体不删+g_toggleOn 陈旧键累积（罕见路径，HWND 复用碰撞可忽略）。
- **symbolpanel.cpp（756 行）全文干净**：\x1F 解析限 12/收藏上限 60 差异持久化/最近条目去重前移；CatAt 索引映射与保存侧 ±1 换算对称；滚轮换分类 mod 循环有界；日期页 6 条目同源取时；右键清空最近/收藏带确认；无痕模式不录最近。微瑕不修：网格上方 4px 填充带因整除截断计入第 0 行命中区（纯命中区微扩，无越界）。
- **clipboardui.cpp（668 行）全文干净**：LXCB 加载全程边界校验（截断文件保留已读部分）；.tmp+MoveFileExW 原子写；回声去重（仅最近一条）+已存在前移+节流 5 次落盘+删除/清空立即落盘（用户意图优先）；GlobalSize 界定 wcsnlen 防非终结数据越界读；SetClipboardData 成功让渡所有权、失败 GlobalFree；监听窗 WM_DESTROY 注销 listener。微瑕不修：WM_CTLCOLOR 返回 WHITE_BRUSH 但 SetBkColor=kUiCard（纯装饰色差）。
- **phraseui.cpp（410 行）全文干净**：行循环 last 先记防收尾漏读；NormalizeAbbrev 白名单 a-z+2-16 位；制表符为存储分隔符故短语剥制表（写读对称）；去重=缩写+短语全等（同缩写多短语合法共存）；写失败 WARNING；g_hooks.phrasesChanged 热载。
- **hotphraseui.cpp（487 行）全文干净**：录制框 KEYDOWN/SYSKEYDOWN 捕获、Esc=清空录制与关窗 Esc 语义分离（注释明示）；加载侧 mods 掩码归一+Ctrl|Alt 强制+vk 非零；键占用去重（mods+vk 全等）；两 EDIT 共享原类过程同份 g_oldHpEditProc 合法（同类控件）；NotifyHotPhrasesChanged 供备份导入侧外部触发。
- **blockedui.cpp（435 行）全文干净**：unordered_set 去重；长度/条数双侧钳制；BlockWord 公共路径统一（窗口添加与候选右键屏蔽同路，结果枚举全分支映射 UI 提示）；清空确认；UnblockWord 逐条精确匹配。微瑕不修（跨四窗同型）：phraseui/hotphraseui/blockedui 的 CTLCOLOR WHITE_BRUSH 色差与 clipboardui 同型。
- 结论：轮 10 收口——帮助窗/符号面板/剪贴板历史/自定义短语/快捷短语/屏蔽词/设置窗全文完成，ui 层深审再无未读文件。本轮 2 真缺陷（裸滚轮误改字号/hover 药丸滞留）全修，轮 2-10 累计真缺陷 **7 处全部已修**。门禁：build Debug 零错 + engine_test **234/0** + fix_readings 缺口 0，全后台（smoke 双套件按 runbook 押后空闲收口）。

### 深度审计轮 11（predictor.h + pipe_util.h + guids + usagestats 全文，收零散尾件，零缺陷）

- **predictor.h（99 行）干净**：纯接口契约无实现——cover 平行数组约定（size()==cands.size() 才有效/元素 0=未知）、Rerank 失败返空=保序、MarkBroken 本会话永久熔断、chain 所有权（析构逐一 delete）语义与轮 5/6/8 已审实现逐条对得上。
- **usagestats.cpp（302 行）全文干净（轮 3 只审过解析段，本轮补全文）**：PrevDayKey 仅以 TodayKey（SYSTEMTIME 来源 m∈[1,12]）入参，文件脏值只做相等比较不进日期回退——mdays 数组无越界路径；"day=" vs "dayPicks=" 前缀 strncmp 索引 3 处区分不串键；BOM 双读法（raw.compare(0,3,...) 短串安全）；.tmp+MoveFileExW 原子写（注释写明撕裂窗口两场景）；节流 20 次落盘+StatsFlush 退出补写；跨天 streak 用纯 YYYYMMDD 回退无时区问题；peakDayChars 实时刷新。
- **pipe_util.h/guids.h/guids.cpp 琐碎干净**：声明与轮 9 已审 pipe_util.cpp 一致；guids.cpp 唯一实例化点 initguid 模式正确（先 <initguid.h> 再本头，其余 TU 只得 extern 声明）。
- 结论：ui/ + common/ + ui/engine/ 全部源文件深审完毕。**发现最后一块空白：ime/（ImeCore TSF DLL）6 文件从未深审**，轮 12 起进入 DLL 侧（dllmain/service/composition/editsession/pipeclient）。门禁：engine_test 沙箱 **234/0**（本轮零代码改动，构建基线承轮 10）。

### 深度审计轮 12（ImeCore DLL 侧：dllmain + pipeclient + editsession + service + composition 全文，1 真缺陷已修 `46bfa75`）

- **dllmain.cpp（347 行）干净**：类厂 QI/AddRef/Release 标准模式；GuidToString 39 字节精确（{+36+}）；注册三步任一失败回滚走 DllUnregisterServer；注销失败传播注册表脏值防幽灵 TIP；DllMain DLL_PROCESS_ATTACH 只 InitLogging+DisableThreadLibraryCalls。微瑕不修：DllGetClassObject new 无空检（分配失败本就失控）、DllMain 内 InitLogging（无消费者无害）。
- **pipeclient.cpp（233 行）干净**：连接状态机 stop-event 全路径退出；Send 临界区整帧串行写+断连 500ms 退避；MSG_CANDS 解析 ReadPod/ReadStr 全界检+cover 可选尾（短帧截断拒收）；断连回调经 PostMessage 封送（HandleMessage 回调在管道线程直调 OnCandidates——刻意设计，避免 WaitForLatestCandidates 死等，DLL 线程模型核心事实）。微瑕不修：Stop 3s 超时后 DeleteCriticalSection 理论 UAF（实际唤醒远快于 3s）；MSG_INSERT_TEXT PostMessage 失败（队列满）堆 wstring 泄漏（极端场景罕见）。
- **editsession.cpp（143 行）干净**：三会话（Read/Insert/Replace）引用计数全配对；GetTextExt 取插入点选区防上下文漂移；SetSelection 统一收敛宿主差异。微瑕不修：CReplaceSession `-(LONG)_deleteLen` 理论溢出（值域同用户 DACL 管道实测 ≤9）。
- **service.cpp（946 行）发现 1 真缺陷（已修）**：`_assocShown`/`_assocReq` 由管道线程 OnCandidates 在 _cs 内写，但宿主 UI 线程 6 处无锁裸访问（InsertCandidate 点选连发读+收尾清、OnTestKeyDown 联想撤销双写、空格认领读、HandlePinyinKey 联想选词道读），且 service.cpp 原注释声称"OnCandidates 皆在宿主 UI 线程"与 pipeclient 实际线程模型不符。修法（`46bfa75`）：按 CandidatesVisible() 房风新增 IsAssocShown()/ClearAssocState() 锁式访问器，6 处裸访问全改走 _cs，修正假注释；无行为变更。其余干净：RequestSession 同步优先降级异步；ActivateEx 失败回退 Deactivate；OnEndEdit 自插入守卫（_fSelfInsert 窗口+_lastSelfInsertTick 400ms/500ms 双兜底）；SendCtx ver 递增；OnCandidates ver 检查+rerank 保 _pageStart+cover 平行数组；WM_APP_LX_PICK wParam=index/lParam=ver 与 HandlePickFromUI(ver,index) 参数序匹配；WM_APP_LX_INSERT delete 恒执行；WaitForLatestCandidates ResetEvent 竞态仅致有界等待退化，非缺陷。
- **composition.cpp（1118 行）干净**：SafeGetTextExt SEH 内纯 POD 局部（无 C++ 展开对象，tfSel.range 释放配对）；组合引用计数全配对（_pComposition/_pRange 增减平衡，AbortCompositionState 先摘引用再 SEH、内层二级异常仅泄漏 COM 引用已注释）；翻页导航全键位（PageUp/Down/Left/Right/Home/End/减号句号）与 _pageStart 权威账一致；空格/Tab 提交道 WaitForLatestCandidates(400ms) 三面门（Test 认领→Handle 提交→InsertCandidate 复验）；无组合模式 PinyinStart/Append/Shrink 内存串路径与组合路径等价。微瑕不修：HandlePinyinKey 联想选词道 `_verCands == _verSent` 在锁外读（InsertCandidate 锁内复验兜底，良性竞态）。
- 结论：**ime/ 6 文件深审完毕，全仓（ui/common/ui.engine/ime）深审收口**。真缺陷累计 8 处全修（轮 5×1+轮 6×2+轮 7×2+轮 10×2+轮 12×1）。门禁：build Debug 零错；engine_test 沙箱 **234/0**；fix_readings --check 缺口 0。DLL 侧改动须宿主重启后真机验证（smoke 双套件+肉眼验收，押后至用户空闲前台收口）。

## 2026-11-01 · 轮 13：三功能批（符号面板搜索框 + v 模式数字大写/金额/计算器）

### v 模式——数字大写/金额/计算器（`c3acaa4`，全拼专属）

- **触发口径**：组合 "v"+calc 字符（IsCalcQuery）；双拼 v=声母(zh) 不触发（`!LoadSettings().shuangpin` 门三处一致：PinyinKeyEaten/HandlePinyinKey/引擎拦截）。误入（"va"）自动脱离，退格删回 "v" 可重触发。
- **ui/engine/calcnum.{h,cpp}（新）**：IntToCn 四位分组（万亿/亿/万单位、零折叠——整组零与低位组前导零合并一个"零"、组值<1000 补零、小写"一十"→"十"归约、前导零剥、16 位上限超限返空跳候选）；RmbFrom 元角分/整矩阵（00→整/30→角/03→零 X 分/0.03→"叁分"/34→角分，全零→零元整）；CalcParser 递归下降（+ - * / 括号一元负号、除零=非法、字面量 1e300 溢出保护）；FormatDouble（近整数折叠、%.6f 去尾零、|r|≥1e16 返空）。纯数字判定的旧实现"无点即纯数字"曾把 "(1+2)*3" 误判为纯数字串（表达式断言全挂），修为全字符数字+至多一个点的逐字符扫描。
- **接线**：pinyin.cpp Predict 入口拦截（cover=整串消耗，提交后全清）+ Learn 拒收 IsCalcQuery 键（防自学习污染词典）；pipeserver.cpp rerank 门加 `!IsCalcQuery`（确定性候选重排纯浪费，省 LLM 调用）；composition.cpp：CalcKeyChar 美式键位映射（无 Shift 数字/OEM_MINUS'-'/OEM_PERIOD'.'/OEM_2'/'，Shift+8/9/0=*()/、OEM_PLUS'+'）、PinyinKeyEaten 在 PinyinActive 分支 switch 前插 calc 门（压住数字选词/翻页/标点路径）、HandlePinyinKey 在 VK_OEM_7 块后插 calc 追加块（空格/回车仍走原 switch：空格上屏首选结果，回车原样上屏表达式）。
- **engine_test P5v 段 13 断言**：触发口径/零折叠（壹亿零伍/壹万零壹拾）/一十归约+前导零/金额矩阵六例/全零/计算器（(1+2)*3、10/4=2.5、负数负前缀）/非法三例（除零/语法错/17 位超界）/引擎集成（Predict 拦截+整串 cover）/Learn 拒收。Report 234→**247**。

### 符号面板搜索框（`1571dc0`）

- **ui/symsearch.{h,cpp}（新，AGENTS.md 新功能新文件规矩）**：自包含关键词表 ~530 条（拼音全拼/中文/英文，与面板分类同序）+ SymbolSearch——空格分词多词 AND、整词 2 分/子串 1 分稳定排序（"mao"：🐱/猫整词在前，"maohao 冒号"子串在后）、结果上限 96（面板 12 行）。emoji 全部 \u 转义（批次 I 规矩：防 VS16 不可见字符编辑丢失）。
- **symbolpanel.cpp 集成（搜索行常驻）**：GridY()=标签行+搜索行+边距，网格整体下移 30px（kSearchH 常驻占位，非搜索态也显示搜索框+CUE 占位）。**激活态机**：面板本体保持 WS_EX_NOACTIVATE（网格点击不抢焦点行为不变）；点击 EDIT（子类化 WM_LBUTTONDOWN/WM_MOUSEACTIVATE）→EnterSearch：临时摘 NOACTIVATE+SetForegroundWindow+SetFocus（真实 EDIT 需要 WM_CHAR 流，NOACTIVATE 下击键全漏给宿主），g_prevFore 锚点；退出五路——Esc 两段（清词→ExitSearch 还前台）/回车上屏首个结果/点标签/网格插入/WM_ACTIVATE WA_INACTIVE 失活兜底（别人抢走前台时静默退出不抢回）。ExitSearch 先置 g_searching=false 再清 EDIT（EN_CHANGE 由闸忽略）再还前台（WM_ACTIVATE 免疫自触发）。**插入链路与焦点无关**（管道 SendInsertText，四管理窗已验证焦点被夺后仍可插入宿主）——这是敢抢前台的架构前提。
- **绘制**：搜索态接管 CurCat()（62×8 结果格，>2 字符长条目 13px 小字号 g_fontTiny 防溢出）；标签选中药丸隐藏（网格区由结果接管）；★收藏角标搜索结果照标；空态三态提示（空查询用法/无匹配/最近收藏空）；滚轮换标签在搜索态禁用（结果滚动留后续）。EM_SETCUEBANNER(0x1501) 按值直发（工程 WINVER 下 winuser.h 未暴露宏）。WM_DESTROY 全量复位（g_searching/EDIT 句柄/子类指针/g_prevFore/结果 store——残句柄是下次开面板的崩溃源）。
- **engine_test P5s 段 7 断言**：整词排序（mao→🐱 首）/拼音整词（douhao→，）/多词 AND（mao tou ying→仅🦉）/中文（爱心→❤️）/颜文字 24 条/空查询/进度条可达。Report 247→**254**。

### 门禁与押后

- 门禁：build Debug 零错（ImeCore.dll+LingxiUI.exe 全链接）+ engine_test 沙箱 **254/0** + fix_readings 缺口 0，全后台（宿主未动，用户使用期零打扰）。
- 真机验证押后（runbook 第 6 步，宿主重启后）：v 模式全链（含双拼不触发/退格重触发/Build32 DLL 侧键位）+ 搜索框焦点流转（激活/五路退出/回车上屏后续打字）+ 视觉走查（搜索行布局/长条目小字号）。runbook 回归预期同步 254。
## 2026-11-01 · 轮 14：前台收口 runbook 执行——smoke5 首跑 12/17 诊断出 1 真 bug + 4 工装坑全修

用户宣告电脑空闲，执行押后的前台收口（宿主重启→build32→regress→smoke 套件）。

### runbook 1-3 步：全量门禁绿

- 宿主重启到新二进制；build32 补齐（Debug32\ImeCore.dll 含 v 模式 composition.cpp 改动）；regress **ALL PASS**（engine_test 254/0 + fix_readings + deep_test + e2e，e2e attempt 1 直绿）。

### smoke5 首跑 12/17——三层根因，一层比一层深

- **工装层 1（H5-H9 五红，脚本驱动法缺陷）**：SetWindowTextW 跨进程赋值不触发 EN_UPDATE（EDIT 输入通知只在 WM_CHAR 用户路径发出）——实时查找链根本没被驱动到，"功能坏了"是假象；配套的 EM_GETSEL out 参跨进程解引用无效（指针在宿主空间写，PS 侧读回恒 0 初值）。修法：TypeInto（EM_SETSEL 0,-1 全选 + 逐字符 WM_CHAR）模拟真实击键；SelOf 空参读返回值（LOWORD/HIWORD 打包）。固化 PITFALLS **#81/#82**。
- **工装层 2（G5 同款，测试模式撞产品语义）**：5 次复制同一 marker 命中"回声去重"（与最近一条相同跳过）且 g_pending 不增——不满节流 5 次永落盘。修法：5 个互异 marker 使 pending 必跨阈值（首个 marker 随任何一次全量序列化上盘）。
- **产品层（真 bug，`9bfe8cf` 修复）**：修好脚本后 G5 仍红——活体探针实证真机宿主的 clipboard.dat 零更新，全部捕获落在 tmp\regress-*\userdata 沙箱目录。根因三乘积：regress 的 APPDATA 重定向 + LINGXI_SETTINGS_NS=test 被 e2e 期间 DLL 重生的宿主**带出测试生命周期**，而 main.cpp 生产路径清 LINGXI_SETTINGS_NS（护设置键）后，clipboardui/autobackup 的惰性沙箱自检全部误判"非沙箱"——剪贴板历史与自动备份写进 regress 临时 APPDATA，用户真实数据停更（与 #64 同类但更阴：改写数据流向）。修法：settings 增 SnapshotTestSpawned/WasTestSpawned 三态快照（单一事实源），main.cpp 清除变量前先快照，两模块改读快照。固化 PITFALLS **#83**。修复验证：regress 复跑后新沙箱 userdata 下仅引擎侧 usage/userdict，宿主零写入；干净宿主 5 连拷真实 clipboard.dat 落盘。
- 工装小坑：git -F 提交标题被 PS 5.1 Out-File 的 BOM 污染（amend 修正），固化 PITFALLS **#84**。

### 真机验证结果（全绿）

- **smoke5 17/17**：G1-G5 无痕门禁（诊断翻转/无痕期不落盘/关无痕恢复记录）；H1-H12 帮助窗查找全链（F3 开查找行/WM_CHAR 输入即跳转 (17,19)/Enter 续找 86/F3 再续 119/未找到反馈/输入复位/Esc 只关查找行/重开状态复位）。
- **smoke1 15/15 + smoke2 8/8** 复跑绿（新二进制 + 干净宿主回归护栏）。
- 门禁：build Debug/build32 零错；engine_test **254/0**；fix_readings 缺口 0；regress ALL PASS。

### 押后（runbook 第 6 步）

- 肉眼验收清单须用户在场：v 模式全链（双拼不触发/退格重触发/Build32 DLL 侧键位）、搜索框焦点流转（激活/五路退出/回车上屏后续打字）、频闪 v2/暗色跟随/帮助 Ctrl+F/无痕开关/剪贴板窗视觉走查。
- 运维修辞：regress 后若宿主为 e2e 派生（剪贴板/备份沙箱停用是**正确**语义），taskkill 重启即恢复生产捕获——runbook 已加"regress 后、smoke 前重启宿主"步骤。

## 2026-11-01 · 轮 15：空闲任务挖出 #83 修漏一例 + smoke1 播种残留口径收口

执行既定空闲任务（runbook 遗留第 5 项）：usage.txt smoke1 播种残留口径确认。用户在场且活跃打字（2:30-2:35 约 80 选词），调查顺带暴露三处活跃用户竞态。

### 调查三发现

- **发现 1（#83 修漏，真缺陷，本轮修复）**：usagestats.cpp:110 LoadLocked 仍惰性活读 LINGXI_SETTINGS_NS——`9bfe8cf` 只修了症状所在的 clipboardui/autobackup。实证：post-fix 沙箱（regress-2665521258）出现 usage.txt（dayPicks=40，e2e 打字期间 1:59:42 落盘）——usage.txt 全仓唯一写者是宿主，铁证 e2e 派生宿主仍在写沙箱、用户真实统计停更。修法同款镜像：改读 WasTestSpawned() 快照。
- **发现 2（smoke1 播种残留，工装缺陷）**：smoke1 直播种真实 usage.txt（无备份）→ 清零 → 删除——用户累计统计每次运行被摧毁（实录：2:00 的运行删了当时文件，用户 2:30-2:35 重新积累 80 选词，若无备份修复下次 smoke1 又清零）。修法：播种前备份 .smokebak（在死盘上执行，零竞态）、清零验证后还原 + 重启宿主重载（清零污染内存态，不重启则下次节流落盘用零值复写还原文件）、残留 .smokebak 下次运行自愈还原。
- **发现 3（活跃用户三竞态，工装缺陷）**：①活宿主先于播种读盘会把真实数据载入内存、播种被无视（懒加载只读一次）→ 备份+播种移到宿主启动前（kill→备份→播种→启动）；②seed day=20261031 昨日值，活跃用户首次选词即跨天翻转 activeDays 45→46 炸断言 → 改播当日动态日期；③累计选词 Contains('5600') 精确匹配被用户实时选词破坏 → 区间断言（5600-5650/21000-21100/活跃天数精确 45），清零检查容忍 ≤10 零星选词。

### 验证与门禁（全绿）

- 修复后沙箱实证：regress-68618896 userdata 的 LingxiIme 目录空——宿主零写入沙箱；真实 usage.txt 未被触碰。
- build Debug 零错零警；regress ALL PASS（e2e attempt 1 直绿）；**smoke1 16/16**（新增第 9 步"还原后重启宿主"检查项）；smoke2 8/8。
- 还原逐字节验证：smoke1 前后真实 usage.txt 完全一致（day=20261101/dayPicks=80/dayChars=145），.smokebak 零残留。

### 押后

- ui/ 余 5 个动作触发型 APPDATA 读写点（blockedui/hotphraseui/phraseui/userdictwnd/statswnd）与 #83 同风险类——写入由用户动作门控（非持续写），待逐窗定 UX 后统一挂 WasTestSpawned 门控。
- 肉眼验收清单不变（runbook 第 7 步，须用户在场）。

## 2026-11-01 · 轮 16：C4a 双层根因（宿主重启竞态 × _active 劫持）——smoke6 24/24 首次全绿

前情：smoke6 连续 4 轮 23/24（唯一红 C4a 搜索态点选上屏）。本轮全量验证先撞回归轮 19/24，借 lingxi-ime.log.old 10359 行完成全链归因，产品+工装双侧修复后 smoke6 首次 24/24。

### 回归轮 19/24 全链归因（lingxi-ime.log.old）

- **宿主重启竞态**：kill 后仅等 800ms 即起新宿主——smoke 的 DLL 在管道就绪前重连失败，撞"无宿主自动拉起"路径自拉第二宿主（pipeclient.cpp:55，15:58:20.120），随后重连退避 ~10s 黑窗，门控在黑窗内 4 试连败。
- **重连风暴劫持 `_active`**：15:58:30.568-33.606 九个用户应用 DLL 集中重连，各自 MsgActivate/MsgPredict 改写 `_active`——A1 raw 落错窗、A3 PICK 送错客户端。
- **发现隐藏劫持向量**：PredictProc（pipeserver.cpp:~524）每次预测都重写 `_active`——不止 Activate，任何后台客户端预测流量都在抢插入/选词目标。
- **归因盲区**：日志行尚无 PID，多进程同写难分归属，只能靠时间戳流推断——归因后第一步补 PID。

### 产品修复

- **log.h**：LogLine/LogLineV 行格式带 PID（`[ts][pid][Lingxi][tag]`）。
- **pipeserver**：ClientCtx 增 pid（MSG_HELLO 上报，pipeclient.cpp:112 一直有值）；AcceptProc 注册/ClientProc 摘除 `_clients` 注册表（_cs 保护）；新增 `PickTargetClient()`——前台窗口进程 PID 匹配客户端（前台=宿主时 25×20ms 短等焦点迁移），无匹配回退 `_active`，决策全落日志（PickTarget fgPid=%u -> pid=%u 前台命中/回退）；SendPick/SendInsertText 改走前台路由——插入/选词不再依赖激活史。
- **service.cpp**：InsertViaWin32Focus 无焦点时落日志（fg=%p tid=%u）。

### 工装修复（tmp/smoke6.ps1，不随提交）

- 管道可枚举前置检查（`\\.\pipe\` 列表含 lingxi-ime-ipc，20s 不见 exit 2）——宿主进程在跑 ≠ 监听就绪。
- 门控每试失败后 Tap Esc 收残留组合再重试；门控最终失败 exit 2 中止整轮；A6 子门控同款。
- 固化无竞态运行程序（PITFALLS #86）：kill → 轮询等进程消失（≤8s）→ 起宿主（LINGXI_LOG=1）→ 等 LingxiUIReady 事件 → settle 1.5s → 管道复核 → 用户静默检查 → 清日志 → 起测。

### 验证与门禁（全绿）

- **smoke6 24/24 首次全绿**（exit=0）：C4a 日志实证 `PickTarget fgPid=20020 -> pid=20020 前台命中 → SendInsertText 已发 chars=2 pid=20020 → [20020][ime] INSERT 收到 chars=2`——宿主侧前台短等给了焦点恢复时间，TSF 首试即成，DLL 重试/Win32 兜底零触发（纵深保留）。宿主 pid=20076、smoke 客户端 pid=20020，PID 标签全程可归因。
- smoke1 16/16、smoke2 8/8；build Debug 零错；regress ALL PASS（engine_test 254/0 + fix_readings + deep_test + e2e attempt 1 直绿）。
- build32：首次 LNK1168（Debug32\ImeCore.dll 被在用 32 位进程锁写）——rename 侧载解锁（#89，.locked 待持锁者退出自清）后完全链接成功，32 位全量编译零错。

### 押后

- 肉眼验收清单不变（runbook 第 7 步，须用户在场）。
- ui/ 余 5 个动作触发型 APPDATA 读写点挂 WasTestSpawned 门控（轮 15 遗留）。

## 2026-11-02 · 深检轮：选词联想 + build gate 全面复盘（中断恢复后盘上实测）

**起因**：代理中断恢复，账面"已落"一律不信，盘上现读复核前期工作（联想 `04d589b`→锁修 `46bfa75`→gate `ee8e810`）。

**核验通过项**（现读+实测）：
- HEAD 树联想链符号齐全（proto/dll/ui/deep 五面）；settings 三面同权（Load 137/Save 197/Diff 258）；UI 六接（IDC/HWND/重置/Load/Apply/mkRow）逐行对上。
- 审计轮 12 锁修质量复核：IsAssocShown/ClearAssocState 房风访问器，六处裸读写全收编。
- UI 侧 ASSOC 兼容承诺成立：HandleCtx 对 flags 无特殊分支（联想=普通非空续写帧，链预测天然放行）；预测线程 blank 空文闸保留且对 pinyin 帧旁路。
- CandidatesVisible()=纯 DLL 账（_cands 非空+对号），**无"UI 超时隐藏不同步"雷**：候选窗本身无自动隐藏（SetTimer 只挂 toast/文字气泡），toast 到期隐藏带 _toastMode 陈旧守卫不吞候选帧（candwnd_core@HEAD 全文走查）。
- gate 落位复核：deep=Debug/engine=Release/:skipbuild 结构完整（HEAD line 34/58/63）；engine.exe 现值实测 254/0 exit0 median=0.419ms。

**深检抓到两枚真问题**（`53142fe` 微修）：
- **P2 `_assocReq` 无失效路径**：被 UI 队列吞掉回包后成野值，误标后续批次（ghost 窗空格误提交首选）→ SendCtx 发非 ASSOC 帧即在锁内作废（请求标记三清除口补齐，回写 PITFALLS #90）。
- **P3 选词道 ver 比较在锁外读**（审计轮 12 收编 _assoc 态时漏网）：同锁块内完成 hit 判定，对齐 CandidatesVisible() 房风。

**边界验证**：IME 单工程构建 OK（rename-aside #89 手法，未碰并行方活跃 ui 域）；engine 254/0 零偏移。deep/e2e 行为级未跑——工作区 11 文件为并行活跃域且跑批会 taskkill 在用 UI，按避让纪律延给静默窗口批；本改语义属边界收敛（T18a/b 活闸常驻把关）。

**观察项（留后续）**：①终验批 regress-100829655 的 OUT 目录 txt 全 0B 而主日志齐全（行为链另有 engine 实测/HEAD 时间戳佐证=非假绿，但重定向截断原因未钉，疑与 #76 同族）；②联想撤销 keepAssoc 未含 Tab/Esc=设计内保守（Tab 空组合本就不提交候选），GAP §11 无冲突。

### 深检轮补记（同日收官）：53142fe 行为级过闸 + 0B 归档钉因

- **行为级补验完成**：`.qa-head` 干净 HEAD worktree 独立批——engine PASS + fix_readings PASS + **deep PASS（T18a/b 活闸在批内绿）**；e2e 批内三连败，事后 worktree 单跑 **exit=0 全链 PASS**（中英 ghost/中文续写/Tab 提交/pinyin settle 逐条在位）。→ **微修语义零行为偏移坐实**。批毕 worktree 已拆除（git worktree remove），并行方 11 件 WIP 全程未碰。

- **0B 归档钉因**：worktree 独立目录批仍全 0B → "同 tick %RANDOM% 撞目录互截"假说被本案证伪为独立次要隐患（撞车真实存在——并发 8×cmd 的 %random% 实测同序列——已用 :mkoutdir 重试修）。真正规律=**detached 进程树的批内重定向不可靠**：外层 fd 日志 608B 判语全真、errorlevel 链全真、exe mtime 全真，唯独批内 redirect 文件 0B；Start-Process/同会话批归档正常。行为模式两例复现+反例对照封死，机制留白——立案 PITFALLS #91，判卷标准改为"主日志+errorlevel+mtime 三联"。

- **regress.cmd 顺带修**：`:mkoutdir` mkdir 原子独占重试 25 次（防同刻双批撞目录名，与 #86 同族的"并行 agent 机"新坑形态）。

## 2026-11-02 数字选词 + 管理窗裁切刀（`1ac01e8`）

- 用户双报障盘上钉因。①英文/续写窗数字键被吞=双层架空：Test 三面门对候选可见的 Tab/数字/Esc 一律认领，但 OnKeyDown 中文模式提前进拼音处理、tempEnglish 一律放行——旧"英文候选 InsertCandidate 数字道"两头堵成死代码；联想态空组合数字道只认 IsAssocShown。修复=统一"无组合候选窗选词道"双落点（space/Tab=本页首选，1-9=第 n，Esc=撤窗；超界数字照常写正文，Ctrl/Alt 直通不变）。
②"用户词典与自定义词条管理没有删除按钮"=布局裁切实锤：四管理窗布局按客户区坐标设计、建窗把**窗口总高**当客户区用，底部整排按钮（删除选中/清空/导出/关闭）被 21+px 标题栏+边框裁到可见区之外——代码文案路由全在，只有渲染在窗外。修复=AdjustWndRect(AdjustWindowRectEx) 助手补齐非客户区；本笔提三自制窗，userdictwnd 功能窗修复同刀但**与并行方该文件 WIP 大改重叠未混提**（编译已进运行版；其落账后外科补提，登记 HANDOFF 待办）。
- 门禁：gate 全批 engine PASS + deep attempt1 34/34（含 T16 Shift/tempEnglish、T13/T15 数字空格回归未破）+ readings PASS；e2e attempt "typing pass-through" 间歇红——同二进制上轮批 PASS、本轮并行 agent 前台活动互扰（诊断行 foreground 漂移/focusDoc=NULL），非本改动面（字母键与我改的选词键集合无交集），留案观察（#86 同族批互踩）。
- 生效提示：DLL 面数字选词需**用户重启在用应用**（TSF 驻留旧 DLL）；UI 管理窗修复本次重启 LingxiUI 即换（运行版=12:3x gate 重建含全部四窗裁切修，已拉起 16036）。

### 数字选词刀后半：活闸闭环与规则互斥修正（`ff54ab9` + `addef29` + `0f83280`）

- T18c 活闸首跑抓出 1ac01e8 的**规则互斥**：撤销门 keepAssoc 不含 Tab → 联想/续写窗按 Tab 先被撤销门清窗、选词道随后 visible=false 永远打不着 = Tab 道事实上死锁。keepAssoc |= VK_TAB 对齐（`ff54ab9`）。
- 活闸自身也回炉：首版断言"Esc 撤销后数字必放行"（onePass）过紧——Tab 点选后 IC 合法连发新联想批，数字命中新批=正当产品行为，时序竞态不入断言；改为"Tab 提交增长 + Esc 无文本副作用"两条稳定判据（`addef29`，deep attempt1 **35/35** 三连实测）。
- userdictwnd 功能窗裁切以 git update-index 外科直提（HEAD 基线+两枚 hunk，+15/-3），零裹并行 WIP（`0f83280`）；deep 活闸同法（+47/-0）。
- **e2e 留案**：本段回归批 e2e 连红在 "LingxiUI NOT ready→direct mode settle" 基建步（deep 同窗 35/35 绿交叉证明非代码面），= 并行 agent 同机跑批 taskkill/重启窗口的 #86 同族互扰；机时静默窗复跑即可，勿误追。
- **本会话全部提交**：`04d589b`(联想)→`868b455`(账)→`ee8e810`(gate)→`a6b3ae5`(账)→`1ac01e8`(选词道+三窗)→`0f83280`(功能窗外科)→`ff54ab9`(keepAssoc)→`addef29`(T18c 活闸)；下一任：并行方 e2e/deep WIP 落账后，e2e 侧叠加"英文 ghost 窗数字键"断言（数字面 deep 只盖到联想/续写批，英文 ghost 依赖 e2e 宿主自插文本基建）。

## 2026-11-02 ·AutoClaw 轮：loomy 中断 WIP 收口 + ghost 活闸验证 + M4.5 KV 增量

接手 loomy 中断现场（cur/LOOMY-HANDOFF.md + 11 件 WIP），收口与验证：

- **用户词典原子操作**（删除复活根治）：引擎持 _udMtx 原子改内存+置脏，唯一写者收敛落盘；FlushUserDict 等收敛后 UI 再刷新；UI 全部管理操作（删/清/加/调频/导入合并）经 g_hooks 直达引擎；备份导入走 UserDictReplaceFromRaw。词典窗 AdjustWindowRectEx 客户区补偿（裁切 21px 根治）。
- **设置页滚轮转发**：settingswheel.cpp 子类化组合框/编辑框 WM_MOUSEWHEEL 原样转发父窗（组合框不再被滚轮误改选中项）；滚动条零范围清零防残留误判；触控板小增量滚存。
- **ghost 数字选词活闸验证**：e2e 用例 t0=30→t2=38、eaten=1 PASS。3/3 失败真因＝上屏后 400ms 自插入抑制窗（Chromium 异步 OnEndEdit 兑底）吞掉紧随的外部编辑——用例先越窗再插；产品语义保留（真实用户 400ms 后自愈）。
- **e2e 会话 IME 状态保全**：开场记录用户 IME、收尾还原（11-01 会话劫持事故根治）。
- **M4.5 KV 增量复用**（llm.cpp）：公共前缀复用 KV + delta decode；打字追加场景 prefill 降为 1-2 token；decode 失败回退全量；_genMutex 串行化根治预热线程竞态。实测：首次全量 578ms → 增量 266-328ms（prefill 消除，余量=生成预算）；相同 prompt 一致性 OK。--probe-llm 增量一致性验证就绪。
- **回归**：build gate+engine+fix_readings+deep 全 PASS（regress-300826204）；e2e 修复后 attempt2 全 PASS（attempt1 假红=#60 前台竞态噪声）。deep_test 对照实验补记：MS 拼音同宿主同样 AV（PITFALLS #19 铁证），组合类断言改 SMOKE 不计分。
- 待办顺延：LLM 语义重排/前缀提交语义/候选翻页/隐身热键/#91 深挖；用户实测拼音与词典管理。

## 2026-11-02 · GAP R6 长尾轮：候选注音 + 以词定字（工作区未提交，HEAD=83a1009）

**候选注音全链路**（UI 进程内闭环，IPC 协议/DLL 零改动，8 文件）：

- 引擎（pinyin.cpp）：候选注音按各自 cover 前缀经同一 EnumeratePaths DP 重分节，全 FULL 音节路径以 ' 连接（jin'tian）；简拼/残段/cover 未知（用户词典 boost/双拼不可逆推）不注；按 cover 去重——同覆盖前缀注音唯一，多数候选共享少数 cover，重分节开销有界。
- 平行搬运（pipeserver）：anno 与 cands/cover 三平行同权——尺寸不齐即清（旧引擎输出兼容）、cap 裁剪、LLM 重排线程按同一排列搬运 reorderedAnno；Show 签名加 anno 默认参（window.h），非拼音路径零改动。
- 绘制（candwnd_core/render）：锁内快照 pageAnno 防撕裂；占宽实测计入（"（注音）"参与最宽行竞争）；OnPaint 主文本后行内绘制，灰阶次级色（dark RGB(170,170,170)/light kUiText2），选中行近白保对比。
- 取证：--probe-pinyin 候选后缀 (anno)，基线全保 + 注音实证；regress ALL PASS。

**以词定字（GAP §1 矩阵行对齐微软）**（composition.cpp + deep T20a/b）：

- eat 侧：无 Shift [ ] 候选可见才认领（数字门旁新闸），不可见回退原【】标点/放行——cnPunct 关时行为不变。
- handle 侧：新 case VK_OEM_4/6——[ 取本页首选词首字、] 取末字上屏即组合收束；emoji 融合候选代理对完整码点防护；B1 cover 部分消耗语义同数字键（整词 cover=全清；"风"单字 cover=feng → zheng 留组合，正是微软语义）；**不 NotifyPicked**——取字≠选词，记整词学习会污染（fengzheng[→风 不该喂"风筝"），与微软"以词定字不计数"口径一致；Shift 态/无候选回退 CommitChinesePunct（【】｛｝不变）。
- deep T20a/b：xi'an + [ → 西（0x897F）/ + ] → 安（0x5B89），expectEaten 一并断言 eat 侧。

**设置页残留**（settingsui.cpp）：WM_SIZE 重排后 + ScrollSettingsTo 滚动后各补 RedrawWindow(RDW_INVALIDATE|RDW_ALLCHILDREN)（连子全失效；父绘双缓冲+COMPOSITED 原子合成无频闪）——缩放残留已修（修后用户未再报缩放）；常规页开关（BS_OWNERDRAW）滚动残留两轮方案（子窗旧矩形补失效 → 连子全失效）均未根治，**用户裁定搁置**（疑 COMPOSITED/CLIPCHILDREN 组合下 SW_SCROLLCHILDREN 不触发子绘；重启口径=本地复现+截图定位）。

**门禁**：build Debug 零错；regress ALL PASS（engine 254 + fix_readings + deep attempt2 37/37——attempt1 唯一红="LingxiUI ready" 宿主就绪环境抖动，T20 两轮均绿 + e2e attempt1 直绿）。

## 2026-11-02 · 联想阶梯呈现轮：GAP §11 "多段呈现"销账

**上轮账已落库**：R6 长尾轮 11 文件分四笔——`22ecc42`（注音 8 文件）/ `064ef49`（以词定字+T20）/ `5ec026b`（设置残留）/ `2c50d88`（docs）；HEAD=2c50d88 工作区净（上条目"工作区未提交"头衔作废）。

**实证钉题**（--probe-chain，LINGXI_LOG=1）：

- 联想帧中文候选仅 1 条 LLM 整句：'今天天气' → [晴，气温20℃-30℃]（532ms 含首载）；英文帧正常多候选（'Today is a nice ' → day. / I hope ...）。
- 机制根源：SplitContinuation（llm.cpp:938）按句读切出[短句][全句]两档，但 DedupCompletions 规则②（前缀对保较短，ngram.cpp:375-378）把该对必然合并——全句档形同虚设；n-gram 中文路已退役（`d5a7d64`），中文联想=LLM 独占，故只剩单条整句。
- DLL 侧交互底座已在（`04d589b`）：空格/Tab/数字选词（service.cpp:781-814）、点选连发 SendAssocCtx（641-656）、任意键撤销（775-791）——缺的只是 UI 候选塑形。

**方案拍板**：三选一（阶梯呈现 / MSFT 严格词级 / 保持现状）用户选**阶梯呈现**——联想帧拆两级：候选①=首段（首个句读前**含句读**，词级颗粒，空格逐段连发）、②=全句原样（一步选整句）。

**实现**（UI 进程内，DLL/引擎去重语义零改动）：

- `StageAssocCands(Prediction&)`（ngram.cpp + predictor.h 声明，纯函数可单测）：只拆候选 0；kStops=中英统一句读集（。？！，、；：,.!?;:）；门槛：首段 2..8 字（含句读）、剩余 ≥2 字、首段查重；cover/anno 原本平行才同步插头（平行不变量）。ngram.cpp 顺带补 `<cwchar>`（wctype 依赖）。
- pipeserver 贯通：PendingPredict 增 `bool assoc`；HandleCtx 传 `(m.flags & CTX_F_ASSOC)!=0`（实际=0x04，proto.h:85；GAP §11 定稿文字写 0x02 系历史陈旧，0x02 已被 CTX_F_CTX_PREFIX 占位）；PredictProc cap 裁剪后 `if (e.assoc && !e.pinyin) StageAssocCands(pred);`——续写帧不拆（规则②对续写帧仍是正确防线），联想帧为呈现层唯一例外。
- ProbeChain 扩展 probe-stage 对照行（main.cpp），证据环闭合。

**验证**：

- engine_test **E9 12 断言矩阵**（两级同现/首段含句读/无句读不拆/尾句读不拆/剩余 1 字不拆/句读首位不拆/首段>8 不拆/首段查重/c0 无切分照旧/多候选只拆 c0/英文同集/cover-anno 平行插头）。
- probe-stage 实证：'今天天气' → [晴， | 晴，气温20℃]；'Today is a nice ' → [day. | day. I hope ...]——阶梯形态闭环。
- 门禁：build Debug 零错；regress-243677780 ALL PASS（engine + fix_readings + deep 37/37 + e2e attempt1 直绿）；探针扩展后 regress-2171224418 e2e 三连假红=前台抢占噪声（foreWnd=CASCADIA，#60/#86 族；attempt 2 功能步全绿仅 profile settle 红）——隔离环境独立复跑（test NS + 沙箱 APPDATA + taskkill + 静置 45s）attempt2 全 14 步 PASS exit=0，判环境噪声而非回归。
- deep T18a 现口径：LLM 续写"你好有什么问题"无句读→活路径按门槛不拆（正确 no-op），拆段逻辑由 E9 单测覆盖——环境数据本身无句读，非缺陷。
- 生效口径：纯 UI 侧——LingxiUI 重启生效（DLL 未动，在用应用无需重启）。

**剩余长尾**：U 模式拆字（待用户定部件数据源：手工策展 vs 外部数据）→ 用户实测拼音与词典管理。




## 2026-11-05 · AI 整句异步追加（期二，工作区未提交）

- 触发限流三重门：音节>=6（SegmentSyllables 自然读法最小切分）+ 候选已出 + worker 停顿窗 200ms；任一破坏不生成。
- 呈现：整句追加在"当前所示列表"末尾（lastSent 基准），anno 补 (AI) 标记；不参与前 5 排序、不自动选中、不改 preedit；cover=raw 全长（选中清全串）；点选整句按 raw 全串学习。
- 陈旧丢弃：ver/gen/sentSeq 三重闸 + lastSentVer 核对；单次预算 1.2s，超时/迟到整条丢；丢弃带连续计数日志。
- 预算隔离：llm.cpp Generate 预算参数化（输入路径 kBudgetMs=300 不动），整句走 GreedyPass 1200ms；detached worker 与重排共用 _rerankActive/_genMutex 串行；投递时 bump rrSeq 防在途重排覆盖。
- 开关 aiSentence 缺省开（注册表 AiSentence），设置页"智能"页勾选行带耗电说明。
- 断言：engine_test P10j 新增 26 条（切分/触发门/陈旧/迟到/追加塑形/合成满载拼音延迟 idle 0.449ms→busy 0.543ms），总读数 519→545 全绿。

## 2026-11-06 · 期二复审：Enter 直拼迟到帧竞态根治

- 缺口：Enter 原文上屏/Esc 取消不发新 CTX，_verSent 停旧值——在途迟到候选帧（重排/AI 整句 worker）同 ver 被 OnCandidates 照收复活候选缓存，下一击 Tab/数字在空组合选词道把陈旧候选插进正文。重排时代就有，整句 worker 把窗口拉宽到 ~1.4s。
- 修复：DLL 侧 ClearCandidates 连同 ++_verSent 作废旧版本（一行，全部终结点同口径覆盖）；窗口侧 Hide 置 _ver=0（迟到 PostShow 不再复活候选窗）。
- 验证（红绿闭环）：pipeserver 加文件门探针（%TEMP% 旗标+"probeprobe" 文本门，600ms 同 ver 迟到帧注入），deep_test T22——无修复 3 轮必红（enter='probeprobe' afterDigit='probeprobe探针迟到'），修复后 41/0 全绿。PITFALLS #98（env 门不可继承改文件门）。
- regress 四套件：engine_test 545/0、fix_readings --check PASS、deep_test 41/0（含 T22）；e2e 环境性搁浅（direct/pinyin mode settle，#19 前台竞态）——A/B 换回昨晚基线二进制同点同数失败，证非本批引入。

## 2026-11-06 · 细胞词库在线下载（真缺栏销账①）

- 管线：URL 形闸（cellfetch_logic.h：https/.lxcell/无 userinfo）→ staging（%TEMP%，≤8MB）→ 完整性锚（显式 64-hex 或同 origin 侧车 .sha256；皆无=fail-closed 拒装）→ 与本地导入同管（ImportCellPackSync 解析/原子落盘/独立复核，零新增攻击面）。
- 出网层复用 updater::DownloadToFile（GitHub 精确白名单+回环演练旁路，update_test T1/T2 原有覆盖）；下载器可注入，engine_test 假下载器零网络全管线钉。
- UI：词库管理窗加在线源行（URL+SHA256 可空+URL 导入按钮，后台线程模型与本地导入一致）；覆盖确认重跑=重新下载校验（不复读旧 staging）。
- 断言：CELLe-1..15 全绿；engine_test 545→560 FAIL=0。

## 2026-11-06 · 皮肤在线主题（真缺栏销账②）

- 包格式 .lxskin（skinformat_logic.h 纯逻辑）：LXSKIN 1 魔数 + name/id/accent 必填 + hover 可选（缺省按每通道 7/8 向白派生，≈内置肤 0.875 配比）+ accent 白字可读性闸（Rec.601 亮度 ≤160，内置最高 112）+ id 严闸（小写/数字/-，1..32，防路径注入）。
- 管线与 CELLe 同型：URL 形闸（.lxskin）→ staging（%TEMP%，≤256KB）→ sha256 完整性锚（显式/侧车 fail-closed）→ 解析验证 → 规范化重写原子落盘 %APPDATA%\LingxiIme\skins\<id>.lxskin → 独立复核逐项比对。下载器可注入；出网层复用 updater::DownloadToFile 白名单。
- 寻址裁决：settings.skinId（wstring，空=内置 skinIndex）——id 寻址与 cellDisabled 同先例，装/删其他皮肤不重排选择；渲染单入口 UiCurrentSkin：命中→下载肤，未命中（已删）→回退内置，越界→0（SK4 语义不变）。候选窗变化检测从 skinIndex 比对改为解析后双通道色值比对（在线 A↔B/删除回退不错检）。
- UI：skinwnd.cpp 管理窗（新功能新文件，settingsui 只减不增）：列表/URL 导入（后台线程同细胞窗 N9 槽模型）/应用/用回内置/删除；托盘 2024 入口。设置页皮肤下拉仅实际改选时清 skinId（防无关保存静默刷掉在线肤）；候选窗右键换肤/直选清 skinId；内置肤名表两处重复合并进 uistyle.h kSkinNames。
- 断言：SK8-1..20 全绿（解析/URL 形闸/假下载器管线/扫描/注册表/resolver/settings 沙盒）；engine_test 560→580 FAIL=0；deep_test 41/0（T22 在线）；update_test 188/0。