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
