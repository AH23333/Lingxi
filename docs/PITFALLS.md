# 踩坑记录（PITFALLS）

> 每一条都是实际踩过、花时间排查过的坑。按"症状 → 根因 → 正确做法"记录。

## TSF / Windows 输入框架

### 1. OnSetFocus 参数顺序是反直觉的 ⭐️（最贵的一坑）
- **症状**：真实应用里切几次窗口后，候选窗再也不出现；e2e 间歇性失败
- **根因**：`ITfThreadMgrEventSink::OnSetFocus(pdimFocus, pdimPrevFocus)` —— **第一参数是"获得焦点"的文档**，第二参数才是失去焦点的。按 (old, new) 直觉命名后，焦点切换时把编辑 sink 挂到了**失去焦点**的文档上，之后的编辑全部漏掉
- **正确做法**：一律以 SDK 头文件注释为准；用 ctx 指针对照日志验证挂载目标

### 2. 键盘事件不路由：AdviseKeyEventSink 的 fForeground
- **症状**：`ITfKeystrokeMgr::TestKeyDown` 返回 S_OK/eaten=FALSE，但我们的 `OnTestKeyDown` 从未被调用
- **根因**：`AdviseKeyEventSink(tid, sink, fForeground)` 第三参数传 FALSE（照抄官方样本）时，CTF 只把按键路由给"前台键 sink"；程序化激活的宿主里 CTF 不会自动设置
- **正确做法**：直通+预测形态必须传 TRUE（激活期间要看全部按键；Deactivate 会解除注册，不影响其他输入法）

### 3. 现代 SDK 与老样本的差异
- `ITfEditSession` 回调方法名是 **`DoEditSession`**（不是 OnEditSession）
- `ITfTextEditSink::OnEndEdit` 签名是 `(pic, TfEditCookie ecReadOnly, ITfEditRecord*)`——老 Win7 样本里的 `(pic, fReadOnly, ecOld, ecNew)` 编译不过
- `TS_SS_DISABLED` 不存在（TS_SS_* 只有 DISJOINTSEL/REGIONS/TRANSITORY 等）。密码框隔离由 TSF 框架层完成：应用禁用 IME 的控件根本不产生事件，无需自查
- 官方样本 Register.cpp 里的 QueryInterface 有 bug（cast 成 `ITfLangBarItemButton*`），照抄会翻车，自己写对

### 4. CTF 注册三连坑
- **非管理员**：CLSID 可以 per-user（HKCU），但 `ITfInputProcessorProfiles::Register` / `RegisterCategory` 只写 HKLM，无管理员一律 **E_FAIL**（不是 E_ACCESSDENIED，极具迷惑性）
- **路径含空格**：InprocServer32 默认值必须写 `"引号包裹的路径"`，否则 COM/CTF 解析截断 → Register 失败
- **键名**：注册表真实位置是 `HKLM\SOFTWARE\Microsoft\CTF\TIP\{clsid}`——**TIP 是单数**（网上资料多写成 TIPs）

### 5. transitory 上下文只支持同步编辑会话
- **症状**：在（经 CUAS 的）非 TSF-aware 应用里上下文读取/上屏静默失败
- **根因**：`TF_ES_ASYNCDONTCARE` 在 transitory 上下文不被接受
- **正确做法**：所有 `RequestEditSession` 用"**同步优先**，返回 TS_E_SYNCHRONOUS 再降级 ASYNCDONTCARE"

### 6. ctfmon 会抢回前台输入状态
- **症状**：程序化 `ActivateLanguageProfile` 成功后几百毫秒，`KeySink OnSetFocus(foreground=0)` 被触发，按键路由失效
- **根因**：ctfmon 监视窗口焦点，周期性为前台窗口应用系统保存的输入法
- **对策**：e2e 等自动化场景需要周期性重新激活对抗；真实用户走 Win+Space 切换器则无此问题（系统自己维护）

### 7. 合成宿主的三明治上下文
- **症状**：e2e 断言读自己的 ITextStoreACP 拿不到 IME 上屏的文本
- **根因**：CTF 可能往文档栈推镜像/三明治上下文，IME 的编辑落在镜像里
- **正确做法**：测试断言一律**从焦点文档顶层上下文经 TSF 读文本**（这才是用户看到的），而不是读自己的存储

### 8. 合成宿主必须有真实窗口 + AssociateFocus
- 只 `SetFocus(docMgr)` 不够：CTF 键盘路由依赖窗口焦点状态。官方 tsfapp 用 `ITfThreadMgr::AssociateFocus(hwnd, docMgr, &prev)`——照做才有按键路由

## 进程与工程

### 9. TSF DLL 的文件锁：能改名、不能覆盖
- **症状**：MSBuild LNK1168 无法写 ImeCore.dll（明明没有任何"我们的"进程在跑）
- **根因**：CTF 把 TIP DLL 加载进所有激活了它的进程（包括正在用的编辑器）；已加载 DLL 禁止覆盖写，但**允许改名**
- **正确做法**：build.cmd 构建前把旧产物 `ren` 为 `.prev` 让位，链接器永远写全新文件；注册表指向路径不变

### 10. 命名管道多线程写帧交错
- **症状**：间歇性消息丢失/解析失败（HELLO/ACTIVATE 偶发消失）
- **根因**：DLL 侧发送来自两个线程（管道读线程发 ACTIVATE、宿主 UI 线程发 CTX），`[u32 len][type][body]` 帧被并发写交错
- **正确做法**：同一连接的所有 WriteFrame 全程持同一把锁

### 11. 先连接后补发状态快照
- **症状**：UI 进程永远收不到 ACTIVATE，候选窗不显示
- **根因**：ACTIVATE 在 `ActivateEx` 里发，那时管道还没连上
- **正确做法**：连接建立后（HELLO 一起）补发 ACTIVATE 快照；同时候选窗跟随"最后发 CTX 的客户端"（正在打字者）而非最后发 ACTIVATE 者

## 工具链

### 12. cmd.exe 批处理与 PowerShell 5.1 脚本都按 ANSI(GBK) 读取
- UTF-8 中文注释的 .cmd 直接解析爆炸（乱码命令）；.ps1 无 BOM 时中文字符串解析报错
- **正确做法**：.cmd 纯 ASCII；.ps1 纯 ASCII 或加 UTF-8 BOM

### 13. 日志文件是 UTF-16LE
- LogLine 直接 `WriteFile(wchar_t 缓冲)`，文件为 UTF-16LE——用 `iconv -f UTF-16LE` 或支持该编码的查看器读，Read 工具会当二进制拒读

### 14. MSBuild 的 PostBuildEvent 是属性不是项
- 放进 `ItemGroup` 报 MSB4232；必须在 `<ItemDefinitionGroup>` 内

### 15. 并发构建互相锁文件
- 多个子代理并发跑同一 solution 构建会互锁 obj/bin → 每个测试工程用私有 OutDir（如 `bin\enginetest\`），且不要把测试工程加进主 sln

## llama.cpp（M4 新增）

### 16. 版本漂移防御
- 采样器 API（llama_sampler_*）跨版本变动大；`llama_decode` + `llama_get_logits_ith` + 自己 argmax 是十年稳定路径
- 结构体参数（model/context params）**必须**用与二进制同 tag 的头文件编译，否则字段偏移漂移导致写错字段
- 运行时 LoadLibrary + GetProcAddress 绑定，任一符号缺失 → 引擎整体降级，绝不崩溃
- LoadLibrary 加载带依赖的 llama.dll 要用 `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`，否则 ggml.dll 等依赖按默认搜索路径找不到

## M5 崩溃排查（2026-10-27，三段式）

### 17. 激活 sink 挂错对象 → 堆损坏崩溃（0xC000041D）⭐️（本次最贵）
- **症状**：e2e 启动即崩 `0xC000041D`（用户回调中致命异常），且无任何 stdout；偶发 `act=0x80004005`
- **根因**：把 `ITfInputProcessorProfileActivationSink` AdviseSink 到 **ITfThreadMgr 的 ITfSource** 上（错误对象）。该调用时而 E_FAIL、时而“成功”，但会写坏 CTF 内部 sink 表；之后**任意堆分配**都会在 ntdll 里 AV（实测在我们 `Deactivate→CPipeClient::Send` 的 vector 分配里爆，极具迷惑性——崩溃点不等于肇事点）
- **取证手段**：e2e 里注册 first-chance VEH，把 RIP/调用栈按“模块名+RVA”写文件，再用链接 map（`/MAP`）离线解析。没有符号时“崩在 ntdll 堆”这类信息完全无法定位
- **正确做法**：文档约定挂在 **ITfInputProcessorProfiles 的 ITfSource**（进程级单例）。但实测即使 QI 已实现该 IID，CTF 仍返回 `CONNECT_E_CANNOTCONNECT`（0x80040202，内部另有约束）——最终方案是**彻底不用激活 sink**：改用按键路径的语言变化检查（`GetKeyboardLayout`）+ KeySink 前台切换/文档焦点/激活置脏，全部零 CTF 重入

### 18. TSF 查询的调用时机：按键分发/激活回调内是雷区，消息泵空闲点安全
- **症状（初判过宽）**：拼音模式下候选窗不出现，SendCtx 的 caret rect 全是 (0,0,0,0)
- **根因**：`GetActiveLanguageProfile` 在 CTF 按键分发（TestKeyDown/KeyDown 回调）内调用会触发**输入上下文栈重算**：多出一个 sandwich 文档、GetTextExt 失效、候选窗锚点丢失。激活回调（ActivateEx/OnActivated）内同理
- **修正后的结论**：不是这个 API 不能用，而是**不能用在那两种回调窗口里**。正确姿势：脏标记触发 → `PostMessage` 到封送窗口 → 消息泵空闲点（WM_APP_LX_MODE）里安全查询。触发源：TIP 激活、文档焦点变化、KeySink 前台切换、语言 HKL 变化

### 18b. 纯 TSF TIP 切换 profile 时线程 HKL 不变——GetKeyboardLayout 判不了语言 ⭐️（真实应用全不激活的根因）
- **症状**：e2e 全绿，但**所有真实应用里 zh-CN profile 打字母出英文补全候选（jimmy），拼音模式从未激活**
- **根因**：灵犀是纯 TSF TIP（无键盘布局绑定），用户通过 Win+Space 切到灵犀 zh-CN 时，`GetKeyboardLayout` 返回的语言**不会变**（仍停留原键盘布局）。用 `LOWORD(GetKeyboardLayout(0))==0x0804` 判拼音模式在真实应用里永远为假。e2e 里的 `SwitchLanguage` 用 `LoadKeyboardLayout("00000804")` 人为创建了真 HKL，把这个问题完全掩盖了
- **正确做法**：用 `ITfInputProcessorProfiles::GetActiveLanguageProfile(CLSID_LingxiTextService)` 查**真实激活档案**（返回我们 TIP 当前激活的 profile GUID，zh-CN → 拼音模式）。外加 GetForeground（是否我们的 TIP 在前台）双保险。hkl/langid 只能当“可能变了”的触发信号，不能当判据

### 18c. GetTextExt 空矩形要当失败处理
- **症状**：候选窗不跟随光标（停在旧位置）
- **根因**：部分应用/上下文（CUAS 宿主、过渡态）GetTextExt 返回成功但矩形全零，按成功处理会覆盖掉兜底
- **正确做法**：GetTextExt 成功但 right<=left 且 bottom<=top 视为失败，逐级兑底：系统插入符（`GetGUIThreadInfo` 当前线程的 rcCaret，客户端坐标转屏幕）→ 零 rect 交由 UI 保持原位不跳动

### 19. 语言切换 churn 期：TSF 框架组合操作脆弱 + 陈旧 store sink DEP
- **症状A**：切语言后立刻打拼音，`StartComposition` 内 textinputframework.dll 读 `-1` 指针 AV
- **症状B**：churn 后 CTF 可能弃用旧 TSF sink 而不通知合成 store；陈旧 sink 的 vtable 指向已释放内存 → 下次授锁时 **DEP（执行堆地址）**
- **对策（产品侧）**：`HandlePinyinKeySEH` 全程 SEH 包裹组合键处理；异常时 `AbortCompositionState`（不 Release 可疑对象，只断开跟踪）并降级回直通。**TIP 的第一铁律：任何框架异常不能带崩宿主**
- **对策（测试宿主侧）**：e2e/deep_test 的 `TestStore::RequestLock` 同样 SEH 包裹（产品应用按契约自管 sink 生命周期，无此问题）
- **症状C（对照实验铁证，deep_test）**：同一合成宿主内切到**微软拼音**打字 → 同样只落一个字母、同样触发框架 AV（被遏制）；多轮 churn 后框架自身 fail-fast（0xC0000409，不可捕获）
- **结论**：组合编辑在合成宿主（简化 ITextStoreACP + CUAS 路径）内对**所有输入法**（含系统输入法）都损坏——宿主限制，非灵犀 bug；灵犀与 MS 拼音行为等价是能拿到的最强验证。组合类断言只做 SMOKE 观察不计分（deep_test/e2e 同策略）

### 20. e2e 每 attempt 删日志文件 → DLL 句柄写已删除文件
- **症状**：多 attempt 运行后日志里 DLL 行与 LingxiUI 行代际混杂、部分丢失
- **根因**：DLL 缓存的日志句柄指向被删除的文件（内容继续写入但新路径看不到）；每个新拉起的 LingxiUI 开新文件
- **正确做法**：取证时以 VEH 崩溃文件 + stdout（每行 fflush）为准；或 DLL 检测日志句柄失效重开（待办）
- **更正（QC 复核）**：现行 `common/log.h` 的 `AppendToFile` 已改为**逐行开关文件**（每次日志行都重新 CreateFile/CloseHandle），不再缓存句柄——本坑的根因在当前实现下已消除；代价是 `LINGXI_LOG=1` 时每行一次开关文件的高频 IO（曾导致拼音延迟 0.8ms→15-44ms 的"性能回归"，见 cur/HANDOFF.md）。判读旧日志时仍按本条原样取证。

## 符号面板与 IPC（2026-10-28）

### 21. ReadFrame 局部缓冲吞掉同批读到的下一帧 ⭐️（连发帧静默丢失）
- **症状**：fakeclient 连续发 HELLO+ACTIVATE 时，服务端只收到 HELLO，ACTIVATE 丢失；真实 DLL 连接后也连续发 HELLO+ACTIVATE（焦点激活可能丢失）
- **根因**：`ReadFrame` 的 `accum` 是**函数局部**缓冲。为减少 ReadFile 次数，读取请求字节数额外 +64；若一次 ReadFile 读入多帧，取走第一帧后，下一帧的多余字节随 `accum` 销毁被丢弃
- **正确做法**：`ReadFrame` 加持久参数 `std::vector<uint8_t>& rxbuf`，取帧后 `erase` 已消费字节、保留多读字节；UI 侧挂到 `ClientCtx::rxbuf`，DLL 侧在 `Run()` 顶层声明、每次重连 `clear`
- **取证**：在 ReadFrame 前后 dump 收到的字节数与解析结果，对比"请求字节数"与"实际收到字节数"即可定位

### 22. 宿主 TIP 自动拉起无参数实例抢占单实例
- **症状**：Stop-Process LingxiUI 后，宿主 TIP 立即拉起一个**无参数**实例抢占单实例 mutex；手动 Start-Process 的 --symbols/--settings 实例因 mutex 已存在而退出，面板打不开
- **正确做法**：主实例创建命名事件（自动重置）+ 隐藏 message-only 通知窗口 + 等待线程；第二实例检测到 mutex 已存在时 OpenEvent→SetEvent 转发命令再退出，由主实例在 UI 线程执行

### 23. emoji 是 UTF-16 代理对，不能按"1 格=1 wchar"
- **症状**：符号面板表情类 emoji 被画成两格（含半字符），点击插入孤立高代理
- **根因**：BMP 外字符（如 emoji）在 UTF-16 中占 2 个 wchar（高/低代理）
- **正确做法**：按码点遍历——`SymCount` 遇高代理（0xD800..0xDBFF）与随后低代理算 1 个符号，`SymAt` 返回码点的 wchar 偏移与长度；绘制与点击换算必须用同一套逻辑
- **遗留**：GDI DrawText 对 emoji 仅灰度渲染，彩色需 DirectWrite

### 24. 主动插入文本后要抑制"上屏即预测"
- **症状**：点击符号上屏后，UI 误弹预测候选
- **根因**：主动插入同样触发 OnEndEdit，被当成用户输入
- **正确做法**：DLL 封送窗口处理 WM_APP_LX_INSERT 时，用 `_fSelfInsert` 守卫包住 `InsertTextAtSelection`，OnEndEdit 检测到该标记则跳过预测

## 词库数据管线与验证（2026-10-28 下午，Loomy 轮）

### 25. pypinyin 单词单读音 → 多音字口语常音整类丢失词典 ⭐
- **症状**：拼音打 shei（谁）完全不出字，只有多音节词里的"谁"能蹭出（用户报"只能识别出双字"）。
- **根因**：build_pinyin 对每个词只取一套读音，"谁"定格书面 shui；shei 只存在于"鹿死谁手"等词的行里，单字精确桶永远查不到。同类缺口实测 **228 字 / 239 读音**（得 dei/血 xie/色 shai/了 liao/还 huan/行 hang/的 di…）。
- **正确做法**：tools/fix_readings.py 用词典数据自身交叉补全（只吃"音节数==字数"严格对齐行、证据词限表前 1/3 频次），幂等可审计；其 --check 挂进 regress 当数据回归闸。**逐词手改数据=复发温床，一律走管线。**

### 26. jieba 现代词结构性缺口；人工词层放 deps/data-src 会被 .gitignore 整类吞掉
- **症状**："自动补全"只能拼出"自动+不+全"；提示词/大模型/验证码整词不存在；"补全"存在但源频率=3（rank 27.8 万，词链 cost 被单字链反超，模糊音也救不了）。
- **根因**：jieba 基础源对现代领域词缺失/频次畸形，两类病一次修：tools/modern_terms.txt（人工注音）+ build_pinyin 的 inject_modern（**只升不降的搬迁置顶**：劣于 band 的摘旧插新、本来就好的原位保留）。
- **连环坑（提交前自查才抓到）**：现代层最初放 deps/data-src——该目录 *.txt **整类被 .gitignore 排除**（大文件不入库策略），数据源不入库=不可复现。人工维护层一律放 tools/（与口语层 spoken_overlay 同目录同惯例）；新文件提交前跑 `git check-ignore -v <文件>` 自证。
- band 插入若按"跳过已存在"实现会漏修频次畸形类（补全自身被跳过）——**幂等语义=先摘后插**，双跑位置不动。

### 27. 数据/代码改动的三级生效：生成物缓存 ≠ 真源、常驻 UI 进程 ≠ 新版、宿主 DLL 需重启
- **症状**：源词典注入且全部断言通过，用户实测"还是打不出/还在复读"。
- **根因链**：① bin\*\data 是**生成物**——engine_test 的 EnsureWordlists **只在缺失时拷贝**，手改源词库后必须删 bin\enginetest\data\zh_pinyin.txt 重同步（本轮"现代词断言假红 3 例"全因此）；② 词典/LLM/链改动跑在**常驻 LingxiUI.exe**（不重启一律无效；回归还会把它拉起成沙盒 APPDATA 实例，**用户学习词会写进临时目录被清**——收口必查 `Get-Process LingxiUI` 的 StartTime 与环境归属）；③ 键盘/组合类在宿主进程 ImeCore.dll，TSF 不热更，需用户重启应用（README 已注）。

### 28. 深度测试红灯先做 HEAD-A/B 归因，再谈"我的回归"
- 本轮多枚"疑似回归"（T12/T13 冷启动红、e2e 首跑 typing pass-through 红）经 `git stash` 换 HEAD 同环境重跑坐实为**既有竞态/跨套件串扰**（真凶是常驻 UI 实例复用+杀进程时序），反而顺出模式检测卡死这个真产品 bug。红→先 HEAD 对照→红同现=既有/不现=我的，带着结论动刀。

## B1 部分提交轮（2026-10-28 晚）

### 29. 候选序 probe 会因模糊音/学习态失效——固定数字键选词的测试必须 pin 索引 ⭐
- **症状**：deep_test T14 按交接记录的 '7' 选"你"，三轮实测分别上屏 立号/理好/利好（全是 n/l 模糊音的 lihao 词）。
- **根因**：交接时的 probe 序没算默认开的模糊音（lihao 词插队），"你"实际在 idx7；且用户词典学习（全量 cover 的错误词）会把学过的错词顶到前面，attempt 间序还会漂。**部分提交选词的学习键是消耗前缀（ni），不会污染 nihao 序——但选错词学的是整条键，会污染。**
- **正确做法**：引擎侧加断言 pin 住索引（engine_test P6c4：你@idx7 首页可达）；deep_test 硬编码同一键，漂移时两处一起红。候选序取证一律以 fresh 沙盒实测为准，不信历史记录。

### 30. deep_test/e2e 日志走 ANSI 管道，中文必乱码——取证用码点 hex 转储
- **症状**：`mid='??' final='??'` 完全无法定位是哪两个字。
- **根因**：wprintf 输出重定向到文件时按 C locale 转换，非 ASCII 全变 '?'。
- **正确做法**：断言前把宽串转成 `%04X,` 码点序列打印（T14 的 hexOf 模式），乱码日志里也能精确取证。

## B4 英文候选/复读过滤轮（2026-10-28 晚）

### 31. engine_test 不编 llm.cpp；--probe-llm 直连 LlmPredictor 不经链路由 ⭐
- **症状**：想给 ChainPredictor 路由/StripPromptEcho 写单测，发现 llm.cpp 根本不在 engine_test.vcxproj（只编 ngram/pinyin/settings）——LLM 相关纯逻辑全都无法进程内断言；而 `--probe-llm` 走 `CreateLlmPredictor()` 直连，链路由/候选过滤/词表优先一概不经——B4 类修复 probe-llm 测了个寂寞。
- **正确做法**：**接缝前置**——LLM 邻接的纯逻辑（链路由、回显裁剪）放 ngram.cpp 并从 predictor.h 导出（`CreateChainPredictor`/`StripContinuationEcho`），llm.cpp 只留装配与生成；engine_test 用 FakePredictor 替身单测路由。真机端到端另设 `--probe-chain`（真实装配链）。判断一个 probe 能不能验证某修复：看它 new 的是哪个引擎、走没走路由层。

## B3 收口轮（2026-10-28 晚）

### 32. 程序化激活 TIP 必须用 ProfileMgr；旧 ActivateLanguageProfile 有"派发就绪"暗窗 ⭐️
- **症状**：deep_test `pinyin mode settle` FAIL（probe_q_eaten=0）连续 6+ 轮全红（T0-T15 全空、e2e pass-through 三连败），同二进制 30 分钟前刚 22/22 全绿——间歇楔死、重试壳都救不回（单 attempt 内永不再自愈的假象）。
- **排查取证（顺序即方法论）**：①InitSinks 日志三 hr 全 S_OK 排除注册失败；②拼音模式=1 且 PinyinKeyEaten 对无修饰字母必吃，锁定"键没到 DLL"；③OnTestKeyDown/OnKeyDown 入口日志（LX_LOG 门控）楔死时零键事件=msctf 没派发；④GetActiveProfile 楔死时返回灵犀自己——"被微软拼音抢占"假设证伪（前台线程 HKL=0x08040804、灵犀不在 WinUserLanguageList 都曾是误导线索）；⑤决定性证据：settle 15 次探测全红的 attempt，1 秒后 T2 起全过——旧 API 激活后 msctf 按键派发有**可变长就绪延迟**（1s～60s+，随 ctfmon/负载漂移），settle 的 3 秒窗口常不够。
- **正确做法**：程序化激活一律 `ITfInputProcessorProfileMgr::ActivateProfile`（Win8+，走完整输入法切换路径，ctfmon 侧状态同步完整；initguid.h 先包含则全部 GUID 直接可用），旧 `ITfInputProcessorProfiles::ActivateLanguageProfile` 仅做回退；就绪探测窗口给足（deep_test settle 15s）；楔死取证三件套：sink 注册 hr、GetActiveProfile、OnTestKeyDown 入口日志——"注册 ✓+active ✓+零派发"即本坑签名。修后 deep_test 6 连 22/22、regress ALL PASS。

## B5 收口轮（2026-10-28 晚）

### 33. 新增测试后断言连环红：先看失败首发位置+单因一致性，再定回归/漂移；修饰键本体绝不能落"unmapped 提交"路径 ⭐️
- **症状A（bug）**：组合中 tap Shift 会把当前**首选误上屏**——`KeyDown(VK_SHIFT)` 落 `HandlePinyinKey` `default:` 的"unmapped 提交首选"回退（该回退本意是标点/未知键收组合，不是给修饰键的）。用户"按 Shift 想打大写"场景组合被打断。
- **修法**：修饰键本体在分发层单列放行。Ctrl/Alt 本体在 KeyDown 时 keystate 已置位、被 `CtrlOrAltDown()` 闸自然覆盖，**Shift 无此覆盖必须单列**（`wParam == VK_SHIFT → *pfEaten = FALSE; return`，放 CtrlOrAltDown 闸后）。weasel 参照：VK_SHIFT 注册为 preserved key 单独处理，不当普通键分发——普通 key sink 架构下的等价语义就是分发层放行。
- **症状B（归因方法论）**：T16 加入后 deep_test attempt1 偶发挂 1~7 个（T14/T15/T10/T12/T16 混杂），像极了新测试污染。取证三板斧：①**失败首发位置**——挂的 T14/T15 在 T16 之前，T16 不可能是因；②**单因一致性**——attempt1 挂 7 个全部与"_pinyinMode 漂移/候选超时"单因一致（vEaten=0、bPass=0、mid=0 都是直通症状），无第二解释需求；③**LX_LOG 取证**——`Shift toggle -> tempEnglish=1/0` 翻转对正常、OnTestKeyDown 的 vk/py/tmpEn/v/fAct 列直接读漂移值。结论：既有 flaky（T14 "你hao"=部分提交后新候选 vs 空格 `WaitForLatestCandidates(80ms)` 竞态→CommitRaw 回退；T10/T12=激活/模式漂移）暴露增多，非 T16 回归，attempt 间自愈后全绿。
- **正确做法**：新增测试后连环红，先问"失败首发在新增测试之前吗？"——在之前直接排除新回归假设；再看失败组能否被**一个单因**解释（能=漂移/竞态，不能=真回归）；最后 LX_LOG 拿状态列实证。别一红就回滚自己的改动。

## R5 稳定性收口轮（2026-10-28 晚）

### 34. PowerShell 嵌套 powershell 时外层双引号先插值 `$env:`——内层命令形同虚设 ⭐
- **症状**：`powershell "$env:APPDATA='C:\...'; Start-Process ..."` 想给内层设 APPDATA，外层先把 `$env:APPDATA` 展开成字面路径，内层收到 `C:\...='C:\...'` 报 CommandNotFoundException，但 `;` 后的 Start-Process 照常执行——**目标进程起来了、环境变量却没设成**，看起来成功实则配置没生效。
- **根因**：外层 PowerShell 对双引号字符串做插值，`$env:APPDATA` 在传给内层之前已被展开；内层根本没收到赋值语句。
- **正确做法**：嵌套 powershell 的命令体一律**单引号包裹**（`powershell '$env:APPDATA=''C:\...''; ...'`），或干脆避免嵌套——直接在当前层 `$env:APPDATA='...'; Start-Process ...` 同进程执行即可（本工具 shell 的 APPDATA 本就是用户真实值，多数"重启真实 UI"场景根本无需覆盖）。检查内层是否真执行了赋值：看输出里有没有赋值语句被当命令名报错的痕迹。

## 32 位 TIP 轮（2026-10-28 晚）

### 35. Win11 24H2 的 SysWOW64\notepad.exe 是商店应用跳板存根；TIP 实例化必须有焦点上下文 ⭐️
- **症状A（假验证）**：探索文建议"用 `C:\Windows\SysWOW64\notepad.exe` 打字看日志 [pipe] 已连接"验证 32 位 TIP——实测存根 3 秒即退（`Start-Process -PassThru` 的进程 HasExited=True），真身是 **64 位**商店 Notepad；后续 AppActivate(死 PID) 失败、SendKeys 打到别处，首测 44 行日志全是 64 位 DLL 发的，32 位验证形同虚设。
- **症状B（DLL 不加载）**：自建 32 位宿主只做 `CoInitialize + ProfileMgr::ActivateProfile`（hr=0）——ImeCore.dll 根本不在进程内，模块枚举 exit=4。**TIP 实例化必须有焦点上下文**：窗口 + `ITfThreadMgr::Activate` + `CreateDocumentMgr/CreateContext(TestStore)/Push` + `AssociateFocus` + `SetFocus`，msctf 才会在本进程 CoCreateInstance TIP（deep_test 骨架即为此而生，裸激活不算数）。
- **正确做法**：验证 32 位 TIP 用 `tools\host32.cpp`（文本宿主+ProfileMgr+模块枚举断言 `dir32=1`，x86 交叉编译 `vcvarsamd64_x86` + `/std:c++17`）；日志归因用"基线长度截取增量"而非行号（LX_LOG 无 PID 前缀）；凡是"启动一个 exe 验证加载"的测试，先核验该进程没被跳板/别名替换（`$p.HasExited`、主模块 PE Machine=014C）。

## 用户六项需求轮（2026-10-28 晚·十）

### 36. PowerShell `Get-Content -Raw` + `Set-Content -NoNewline` 会把 UTF-8 中文源码写成 ANSI 损坏文件 ⭐️
- **症状**：批量文本替换用 `$c = Get-Content -Raw -Encoding UTF8; $c.Replace(...); Set-Content -NoNewline -Encoding UTF8`——写回后中文全乱码（源文件被 ANSI 重编码）。
- **根因**：PS5.1 的编码参数链有坑：`Get-Content -Encoding UTF8` 读对，但经变量中转再 `Set-Content` 时 BOM/代码页处理出错，中文字符按系统 ANSI 码页重写。
- **正确做法**：**批量文本替换一律用 edit 工具**（精确 oldString/newString），不用 PS 字符串替换写回源码文件。

### 37. P/Invoke FindWindowW 传 `$null` 标题封送成空串 `""` 而非 NULL 指针 ⭐
- **症状**：`FindWindowW($className, $null)` 按标题精确匹配失败，误报"窗口不存在"。
- **根因**：PowerShell 把 `$null` 字符串参数封送为空字符串引用（非 NULL）——窗口标题非空则永不匹配。
- **正确做法**：枚举顶层窗口用 `EnumWindows` + 回调里 `GetClassNameW` 匹配类名；判存在性别依赖 FindWindowW 精确双匹配。

### 38. 冷启动（LLM 加载）期间触发二实例命令：转发器 4s 超时弹错误框挂进程 ⭐
- **症状**：taskkill 后重启 UI 立即 `--settings` 触发——主实例还在 LLM 冷加载（CPU 30s+），命名事件未创建，转发器 SignalNamedEvent 4s 重试耗尽弹"仍在启动中"错误框，进程挂在那里不退；后续烟测进程数判断全乱。
- **正确做法**：冷启动后**等就绪再触发**（本机 LLM 冷加载实测 45s 稳妥）；烟测脚本轮询判据用"目标窗口类出现"而非进程计数（转发器存活会污染计数）。

### 39. 探针字符串插值 `-band` 表达式字面输出——断言"假绿" ⭐
- **症状**：`"WS_VSCROLL={(((351207424 -band 0x00200000) -ne 0))}"` 输出原样表达式文本，肉眼看带花括号以为 True——**样式断言根本没算**，得出错误结论。
- **根因**：PS 双引号字符串里 `{...}` 是 `-f` 格式化占位符语法，不是子表达式；嵌套圆括号不触发求值。
- **正确做法**：先用变量算布尔再插值（`$v = ($s -band 0x200000) -ne 0; "WS_VSCROLL=$v"`）或用 `-f` 运算符；凡是探针输出里看到裸表达式文本=断言失效，必须返工。

### 40. 依赖环境 HKCU 的测试必须开头 pin 前置；edit 批量替换要带唯一上下文 ⭐
- **症状A**：deep_test T16 挂三连——环境 `ToggleMode=4`（设置页烟测点组合框留下的），Shift 切换语义不成立；D 轮 regress 时环境还是 1 所以全绿，污染时序一变测试就红。
- **根因**：DLL 每事件实时读 HKCU（`LoadSettings()`），测试隐式依赖环境状态；烟测动真实设置页=动测试前置。
- **正确做法**：环境依赖型测试在块开头显式 pin（T16 已修 `c00e972`：PinToggleMode(1)+结束 RestoreToggleMode，DLL 实时读即写即生效，崩溃残留=默认值无污染）。
- **症状B**：edit 工具改 T16 时 oldString 用 `EnsurePinyinMode(pKm);\nClearFocusedText(pTm);`——该模式在文件里出现 15+ 次，居然"成功"落进了 T15 的块，编译错误才暴露。
- **正确做法**：编辑重复模式必须带唯一锚（注释行/独有变量名/相邻独有语句），多匹配"成功"≠落对位置。
