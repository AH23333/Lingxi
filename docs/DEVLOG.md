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
