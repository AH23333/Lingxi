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

## R3 多路采样轮（2026-10-28 晚·十一）

### 41. 后台负载使 regress 时序预算全漂——判归因先查 CPU 消费者，再用 stash 旧码 A/B ⭐
- **症状**：`jintianqi` 拼音时延（预算 10ms）同二进制一晚漂 7.2~14.7ms（中位），regress 单次全绿困难；deep_test 三轮失败项各不相同（T14/T15/T16/T10 散布，AV=0），e2e `focused=''` 时绿时红。
- **排查与定性（方法即结论）**：① `Get-Process | Sort CPU -Desc` 抓真凶——**left4dead2 游戏在跑 + 残留 LingxiUI 的 LLM 预热（启动即烧 ~45s CPU）**；② `git stash` 旧码重建复测——旧码同样 7.2→13.7→14.2ms 漂移，同分布=环境而非代码；③ 残留 UI 杀掉后 engine_test 立即 7.07ms 全绿。
- **正确做法**：时序预算红先查后台（游戏/Defender 首扫新 DLL/残留 UI 预热），`taskkill /IM LingxiUI.exe` 后复测；仍红再 stash A/B 旧码定责；同二进制跨越绿红两侧的漂移一律判环境。**测试机上有前台娱乐负载时，紧预算（<2× 空载基线）项不可作门禁**。
- **附**：每个 LingxiUI 实例启动都触发 LLM 预热全速烧 CPU——regress 内 deep_test/e2e 交替拉起 UI，相邻测试彼此制造负载，单次全绿有运气成分；分组件绿+跨轮次绿+AV=0 即可收口。

### 42. 残留 LingxiUI 跨 taskkill 存活污染后续 e2e——测前必杀，全绿前不信清理 ⭐
- **症状**：e2e `typing pass-through` 九连败 `focused=''`（插入成功但读回空）——残留 UI（PID 29216）在 e2e 三实例间存活；单独复跑（先 taskkill）立即全绿。另一残留（PID 4232）同时把时延推高 5ms。
- **根因**：e2e 退出时 `taskkill /IM LingxiUI.exe /F` 偶发未杀净（输出被吞看不见失败）；后续 e2e 的 UI 二实例经转发器挂到残留上，CTF/TSF 焦点链被搅。
- **正确做法**：e2e/regress/deep_test 前一律显式 `taskkill /f /im LingxiUI.exe`；判断"是否有残留"用 `Get-Process LingxiUI` 而非看日志；连续失败先做一次干净重启（杀残留）再下结论。

### 43. （同 #32 案，本条给出根治全貌）测试直写真实 HKCU 设置：一套回归能把用户输入法刷成"全瘫直通" ⭐
- **症状**：深夜全绿 regress，隔天 HEAD 纯文档提交后全线红（engine 18 项挂、latency 0.3→11ms、模糊音全灭、deep settle/T15 全红）——像代码回归，但 git log 现读证明没有代码提交。
- **根因**：此前测试（半程退出/异常销毁）把真实 HKCU\Software\LingxiIme 能力位整体刷 0（PinyinEnabled=0，用户输入法当场瘫成直通）。词典侧早有 APPDATA 沙盒，设置侧没有等价物——测试进程与用户共享同一条注册表轨道，互踩只是时间问题。
- **正确做法**：`LINGXI_SETTINGS_NS` 环境变量切换基键到 `Software\LingxiIme_<ns>`（角色白名单校验，Load/Save 同一入口 ActiveKey()）；三个测试程序启动自设 NS 并清空沙盒键——**缺键=结构默认=断言基线**，免预写；regress.cmd 注入双保险。硬判据=**回归前后真实 HKCU 快照逐字节一致**（10-29 实跑坐实：全链 ALL PASS 且注册表零改动）。收任何新设置键时维持"结构默认即基线"不变式；测试永远不写默认键。

### 44. 设置"整页回写"是刷零发生器——沙盒只解决谁在写，还要堵为什么能整页写 ⭐
- **症状**：真实 HKCU 设置位两次被整体刷 0（用户输入法瘫成直通）。第一次归因测试半程退出（#43 已隔离测试侧），**第二次在测试沙盒已就位后仍复发**——说明产品自身有路径。
- **根因**：设置页 SaveFromControls 的模式是「读磁盘现值 → 用**全部控件当前态**覆盖 → 整页回写」。只要任何一次触发发生在控件未与设置同步时（首建未加载/隐藏期事件/异常半程），磁盘真值就被控件默认态（全 unchecked）整页覆盖。测试沙盒只是写者之一，这个**放大器**不堵，任何写者都能造成全清。
- **正确做法**（04ac2fb 双保险）：① loaded 门——LoadToControls 完成前拒写；② **增量落盘** SaveSettingsDiff——与磁盘现值逐字段比对，只写差异键。任何「从 UI 状态保存配置」的功能都适用此规：**未变字段绝不落盘**。

### 45. agent 会话 ≠ 用户桌面会话：Local\ 命名对象跨会话不可见，会把"程序正常"误诊成"程序没启动" ⭐
- **症状**：设置窗转发排查中，Get-Process 明明看到 LingxiUI 活着，但 OpenEvent("Local\LingxiUI_ShowSettings") 返回 0、日志文件不存在——一度误判"新构建启动即崩"。
- **根因**：该实例是**用户桌面会话**里宿主进程（TSF DLL EnsureUIRunning）自动拉起的；agent 的 PowerShell 在另一会话。`Local\` 前缀命名对象（事件/互斥体）**按会话隔离**，跨会话不可见；`\\.\pipe\` 命名管道则跨会话可用。日志"不存在"同理——用户会话实例的 LINGXI_LOG 环境本来就没开。
- **正确做法**：诊断常驻程序前先问"这个实例是谁拉起的、在哪个会话"——同会话自己 Start-Process 的实例才有观测通道（事件/日志/窗口枚举都可靠）；跨会话只剩进程表与全局对象可用。转发/单实例类问题**优先在同会话复现**（本会话早期同会话转发成功有截图为证），别在跨会话残影里追鬼。

### 46. TSF 真机按键门控：OnKeyDown/OnKeyUp 只在 Test 声明 eaten 后被真实应用调用，deep_test 直调绕过——测试绿真机死码 ⭐
- **症状**：Shift/CapsLock 中英切换逻辑齐全（簿记+翻转+toast），deep_test T16 直调 pKm->KeyDown/KeyUp 全绿；真机（WT/记事本）按键毫无反应。
- **根因**：真机 TSF 分发（CUAS/TSF-aware 宿主）先问 OnTestKeyDown，**只把声明 eaten 的键交给 OnKeyDown**；OnTestKeyUp 同理。Shift/Ctrl 在 PinyinKeyEaten 永不声明、OnTestKeyUp 恒返回 FALSE → 真机 OnKeyDown/OnKeyUp 全程不被调用，切换逻辑是死码。deep_test 直调 pKm->KeyDown 绕过了这道门控，合成测试无法暴露。放大器：穿透键（大写字母、Ctrl+Shift+K 的 K）真机也永远到不了 OnKeyDown，`_shiftUsed` 和弦检测同样失效。
- **正确做法**（e6c3d89）：切换键在 Test 阶段 ToggleKeyClaim 主动认领（Shift=mode1/2、Ctrl=mode2/4、Caps=mode3）；按住期间任何其他键**在 Test 阶段**标记 used；OnKeyDown 对认领键统一 eaten=TRUE 返回（Test 吞/KeyDown 放不一致会让宿主只见半程消息）；OnTestKeyUp 与 KeyDown **对称认领**（宿主按消息跟踪 Shift 才不滞留下压态；GetKeyState 类检测不受消息过滤影响，认领安全）。OnSetFocus 复位簿记自愈丢 KeyUp。**规约：任何"按键行为"功能都必须过 Test 门控设计，deep_test 直调只验证逻辑不验证分发。**

### 47. 词典窗索引漂移：引擎学习并发重写 userdict.txt，按列表索引删/调会落空或删错
- **症状**：用户词典"能加不能删"——添加有效，删除选中毫无反应（sel<0 或索引与文件错位时静默）。
- **根因**：引擎每次选词学习即落盘重写 userdict.txt（学习即持久化），词典窗列表展示与删除之间文件随时被并发重写；LB_GETCURSEL 的索引在删除瞬间对应了另一个条目或越界。删除失败还无任何提示（sel<0 直接 return）。
- **正确做法**（c93121e）：删除/提升/降低改按**内容匹配**（列表行解析 word+key，count 可变不参与匹配），失败回退索引路径；sel<0 明示"请先选择词条"；删除后重选相邻行并明示结果。**规约：UI 列表操作一个会被后台并发重写的文件时，索引只是渲染产物，操作必须锚定内容。**

### 48. C/C++ 注释行尾反斜杠的行拼接把下一行吞进注释——C2065 假报"标识符未声明"
- **症状**：`slash[1] = 0; // ...\bin\Debug\` 下一行 `wchar_t script[...]` 声明，编译报 C2065 "script 未声明"。
- **根因**：行尾 `\` + 换行 = 行拼接（line splicing），物理下一行成为注释延续，声明整行被吞。注释里的 Windows 路径习惯性以 `\` 结尾最易踩。
- **正确做法**：注释里的路径永远不以反斜杠结尾（用 `/` 或省略尾分隔符）；遇到"声明明明在却报未定义"优先查上一行行尾。

### 49. e2e 与真机用户抢 ctfmon 前台：用户实时切换 IME 会把 e2e 激活的配置拉走，typing pass-through 三连红
- **症状**：大修验证轮 e2e 三 attempt 全红在 typing pass-through；细节 attempts 2/3 显示 `foreground = other TIP`，而 ActivateLanguageProfile 本身 PASS。
- **根因**：SetActiveProfile 是桌面级全局状态；用户正在真机测试新构建（活跃切换 IME），在 e2e 激活与打字断言之间把前台配置切走。同 binary 等待复跑即全绿（attempt2 24 PASS/0 FAIL）。
- **正确做法**：跑 e2e/deep_test 避开用户活跃窗口（此前 #19 只记录了 ctfmon 竞争负载维度，本条是前台配置被主动切换维度）；连续多 attempt 同点红且伴随 foreground 漂移时，先怀疑环境再怀疑代码。

### 50. 沙箱变量的逃逸方向是反的：测试宿主内 DLL 按需 spawn 的生产 LingxiUI 继承 `LINGXI_SETTINGS_NS`，把用户设置静默写进沙箱键 ⭐⭐
- **症状**：用户报"设置的大多功能都不生效，修改无效"；真实键 `HKCU\Software\LingxiIme` 只有 deep_test pin/restore 留下的 2 个孤儿值（ToggleMode/ShiftToggle），用户 19:40 的全部 17 项修改落在 `LingxiIme_test` 沙箱键里。
- **根因**：`LINGXI_SETTINGS_NS` 是进程级沙箱（#43），本意隔离"测试写真实键"；但 deep_test/e2e 进程内的 TSF DLL 会按需 CreateProcess 拉起 LingxiUI（候选窗服务），子进程继承测试环境 → **生产设置 UI 带着沙箱变量启动**，SaveFromControls 全部写进 `LingxiIme_<ns>`，引擎（真实应用进程，无该变量）读真实键 → 永远读不到用户的修改。沙箱机制防住了"测试污染生产"，却制造了"生产被测试环境吞掉"。放大器：单实例互斥锁让沙箱实例可能长期占位（孤儿存活到下一次 taskkill）。
- **取证手法**：①注册表键 LastWriteTime（P/Invoke RegQueryInfoKeyW）对齐事件时间线——真实键 19:31:22=deep_test pin、沙箱键 19:40:31=用户保存、用户词典 19:40:39=用户真在打字；②读活进程环境块（NtQueryInformationProcess→PEB→ProcessParameters→Environment）确认幸存实例干净、写入者另有其"进程"。
- **正确做法**：生产入口强制清除沙箱变量（main.cpp 在 probe 分发后、单例前 `SetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", NULL)`——probe 子命令保留沙箱因 e2e 断言依赖）；deep_test 的 PinToggleMode/RestoreToggleMode 同步改走 ActiveKey 同口径沙箱键（旧实现直写真实键，违反自己 #43 宣称的"真实 HKCU 从此不共轨"）。**规约：进程级沙箱变量防不住"子进程继承"这条路——生产可执行文件必须在入口自证清白；涉及沙箱的测试工具连带检查"它 spawn 的一切"。**

### 51. settingsui.cpp 的导出函数放进匿名命名空间=内部链接，跨文件调用直接 LNK2019 ⭐
- **症状**：window.cpp 调 `lingxi::UserDictHasWord`/`lingxi::ApplyCandidateDictOp`，链接报 LNK2019 无法解析的外部符号；声明在 settingsui.h、实现明明就写在 settingsui.cpp 里。
- **根因**：settingsui.cpp 的 helpers（UserDictPathW/ReadFileBytes/WriteFileBytes/ParseUserDict/SerializeUserDict/InferPinyinKey 等）都住在**匿名命名空间**里，按惯性把新导出函数也写进了同一区域——匿名命名空间成员是内部链接， extern 声明对不上，链接器当作不存在。
- **正确做法**：导出函数实现在 settingsui.cpp 必须放在匿名命名空间（`} // namespace`）之后、文件尾 `} // namespace lingxi` 之前的具名区域；匿名命名空间内的 helpers 对其依然可见，不影响复用。**规约：往"helpers 全在匿名命名空间"的文件里加导出函数时，先定位匿名命名空间的闭合大括号再落笔。**

### 52. 周期 SetTimer 的"放弃"分支只打日志不 KillTimer：重试预算耗尽后定时器照样每拍回调——永久空转泄漏 ⭐⭐
- **症状**：LINGXI_LOG 日志 97 秒刷出 390 行 DetectPinyinMode"放弃"（≈4 行/秒）——250ms 周期定时器在重试预算耗尽后从未停止；宿主进程每秒 4 次 TSF 前台查询，与设计注释"不会在正常场景空转"直接矛盾。
- **根因**：`SetTimer` 是**周期**定时器不是 one-shot；DetectPinyinMode 耗尽分支只打日志 return——"放弃"实际是"以原节拍永远重试"。通病：写"预算重试"代码时把 `return` 当成了终止。放大器：无 OnActivated/ITfTextInputProcessorActivationSink 实现，纯 TSF profile 切换不改线程 HKL（#18）也不回调 DLL 刷新预算——注释宣称的"待按键触发重检"名存实亡。
- **正确做法**：耗尽分支降级为慢速自愈轮询（SetTimer 1000ms + `_modeSlowRetry` 标志 log-once 降级日志），premature 判定每次仅进程内 GetForeground µs 级查询（CoCreateInstance 仅 weForeground 时执行），前台回归后 ≤1s 自愈进位；恢复分支 KillTimer+复位预算。选降级而非 KillTimer 硬停：负载机 ctfmon 过渡可超 2s 快速预算，硬停会把 settle 假红引进来。**规约：周期 SetTimer 的每个"不再快试"分支必须显式改节拍或 KillTimer——`return` 不会替你停定时器。**
- **配套判读法**（LINGXI_LOG 干扰期三指纹）：①attempt 日志 **0 条 [key] 行**=探键根本没到本 DLL（前台被其他 TIP 持有，环境干扰非代码回归）；②"放弃"行风暴=本条定时器泄漏；③降级日志恰 1 条+总行数很少=慢速轮询按设计工作。

### 53. Learn 在管道读线程持锁做磁盘 I/O + 全量索引重建：Predict 同锁被饿数百 ms，选字后提交键退化裸字母 ⭐⭐
- **症状**：deep_test T10 间歇 badRound（第 N 轮空格提交字母原文而非"今天"）、T14 数字选字后空格提交裸"hao"（final=你,h,a,o）；多轮 attempt 一致复现，负载越高越稳。DLL 日志里提交"凭空"出现（no-inline 模式 PinyinCommitRaw 无日志行）。
- **根因**：每次选字 NotifyPicked→ApplyLearn→`_pinyinEngine->Learn` 在**管道读线程持 `_udMtx`** 执行 SaveUserDictLocked（全量原子盘写）+ BuildUserInitIndex（对每个 userdict 键跑 EnumeratePaths，~200 键≈300-600ms）；Predict 热路径同锁两次 → 选字后的预测被饿。空格路径 WaitForLatestCandidates(80) 超时 → GetCandidateEx 失败 → PinyinCommitRaw 提交裸字母。空闲基线 9.6ms 完全无感——**典型"低负载全绿、高负载越过预算"的隐性锁竞争**。
- **取证手法**：日志行加 HH:MM:SS.mmm 时间戳后逐帧配对 SendCtx ver=N → 收到候选 ver=N：ver=118 间隔 464ms（对照 ver=117 仅 26ms），失败帧=选字后首帧；锁定"选字→Learn→下一帧预测"因果链。
- **正确做法**（75e4ee9 双管）：①锁内只做内存词典更新+简拼索引**单键增量**（UpdateUserInitIndexLocked——保持"新词即时可简拼调出"同步语义，µs~ms 级），磁盘落盘移后台 detached 线程（_persistBusy CAS 防两入口并发：管道读线程 MSG_NOTIFY_PICKED 与 UI 线程鼠标拾取回调都可能调 Learn；快照即清脏防写期间新学习丢账，写失败恢复脏位保留重试语义）；②提交键等待预算 80→400ms（候选就绪时 WaitForLatestCandidates 零开销，仅 pending 时兜底）——最坏键延迟优于提交错误文本。**规约：读线程回调里绝不持跨线程热锁做 I/O 或 O(n) 重建；同锁的热路径调用方有超时退化语义时，退化路径的"错误输出"（裸字母）必须也留日志。**

### 54. taskkill LingxiUI 后 orphan 宿主 1 秒内抢 respawn 无 env 实例占单例——deep_test 永远拿不到 UI 侧文件日志 ⭐
- **症状**：LINGXI_LOG=1 跑 deep_test，DLL 侧日志文件齐全，UI 侧（[py]/[ui] 引擎日志）永远缺失；手动 Start-Process 带 env 的 LingxiUI 又被单例弹掉。
- **根因**：taskkill LingxiUI 只杀 UI 进程，**加载着 TSF DLL 的宿主应用还活着**——DLL 的 EnsureUIRunning 探测到 UI 不在，1 秒内 respawn 新 LingxiUI；这个实例继承的是**宿主应用**的环境（无 LINGXI_LOG），抢先占住单例互斥锁；deep_test 随后拉起的有 env 实例被单例机制弹掉 → 文件日志永远来自无 env 实例=永远没有。
- **取证手法**：日志文件本身是 **UTF-16 无 BOM**——Select-String 直接搜会漏/乱，必须 `[System.IO.File]::ReadAllLines($p, [System.Text.Encoding]::Unicode)` 读；两实例竞争与否看日志里 [py] 行有无即可判定（DLL 与 UI 同文件追加写）。
- **正确做法**：需要 UI 侧日志时先让 env 实例占住单例（taskkill 后立即以 env 拉起，抢在 orphan 宿主 respawn 之前；或先杀掉持有 DLL 的宿主）。**规约：单例程序被外部 respawn 时，"谁先占坑"取决于宿主环境而非启动者意图——带环境变量调试单例服务，先清占坑者。**

### 55. FindWindowW 跨 window station 恒返回 NULL——同 Session 不等于同窗口站，观测通道要选对 ⭐
## 56. Test 三面门与选词键词表不同步=空格静默黑洞；外加 deep 旧 exe 假绿双坑 ⭐
- **症状**：联想候选窗（空组合）可见时按空格——既不上屏词、也不写入空格字符，键凭空蒸发；T18a 连续三轮 FAIL 却看不出窗口有无候选。且期间两次 regress "ALL PASS" 的断言计数与 T18 插桩前一致——**跑的一直是旧 deep_test.exe**（deep_test.vcxproj 不在 sln，tools/build.cmd 不编它），"新用例没执行"伪装成"新用例通过"。
- **根因 A（空格黑洞）**：TSF 分发律=Test 声明 eaten 的键才有 OnKeyDown（#46 同源）。OnTestKeyDown 三面门 `visible && IsSelectionKey(wParam)` 的选词键词表只含 Tab/1-9/Esc 不含 VK_SPACE（当年为护英文补全 ghost"空格=入字"裁决）；空组合下 PinyinKeyEaten 的 `case VK_SPACE` 仅在组合中被吃。两层都放行 → 宿主侧深测 text store 又不吃空格消息 → 零输出。**跨 Test/KeyDown/handler 三层的键语义必须同闸同步**：新增"候选态可提交键"时要同时审三面门词表 + OnKeyDown 处理道 + 宿主直通假设。
- **修复 A**：三面门只对 `_assocShown`（联想态）额外认领空格（英文 ghost 行为冻结不动）；HandlePinyinKey 空组合分支新增联想选词道复用点选路 InsertCandidate（replaceLen/自学习/连发语义零复制），数字键经既有三面门走同一条道。
- **根因 B（旧 exe 假绿）**：**仓库的测试资产不在一个构建入口**——sln 只含 common/ImeCore/LingxiUI/e2e，deep_test/engine_test 各有独立工程文件。改 deep_test.cpp 后 build.cmd"零错误 ALL PASS"完全可以是旧二进制复读。**防假绿唯一硬证=目标 exe 的 LastWriteTime 晚于源码修改**；新工具工程进回归前必须先补它的构建入口。
- **根因 C（作用域顺手坑）**：既有 suite 把 hexOf15 之类 helper 定义在块内 lambda，新用例块放外层编译报 C3861——照抄名字直接再定义一份本地副本（hex18），别抬到文件域重构旧代码。
- **纪律固化**：T18 绿灯链=改 deep → 跑 tools/deep_test 独立构建 → 核 exe 时间戳 → regress。跳过任一环都可能把"没跑"读成"通过"。
- **✅ 已根治（`ee8e810`，2026-10-30 深夜）**：regress.cmd 内置 build gate——sln+deep(Debug)+engine(Release) 三条编译全部自跑，build 失败 exit 1 绝不带旧二进制开测；时间戳人肉核对被 exit 门替代。根治当晚即抓到连锁第二例：同一 engine_test，Debug 配置（/Od+_DEBUG 迭代器检查）latency median≈17ms 全红，Release（README §引擎单元测试 + cur/HANDOFF 白纸黑字的历史基线"0 FAIL（Release）"）亚毫秒真绿——**构建配置本身是断言语义的一部分**，跑批器写错配置=整条性能断言失真。vcxproj 实含 Debug|x64 与 Release|x64 双配置（首轮侦察用 `|Release` 反查串写错导致误判"无 Release"，现读纠正）。

### 57. 多行为者竞态：Get-Process 拿到的进程不一定是自己启动的——旧码进程伪装"修复无效" ⭐⭐
- **症状**：SB_TOP/SB_BOTTOM 修复构建成功（build.cmd exit 0 ×2）后对设置窗实测"无效"，险些误判修复错误返工。
- **根因**：机器上存在并发行为者（用户/残留自动化），会在你 Start-Process 之外的时刻启动 LingxiUI；拿到焦点的窗口可能属于一个加载**修复前 exe** 的进程。构建成功 ≠ 运行进程已换新（本轮 12816 竞态旧码进程即此）。
- **判别法（实证）**：①核 `proc.StartTime` vs `exe.LastWriteTime`——进程起于构建前=旧码；②**新 case 判别法**：修复新增的行为（如 SB_TOP 响应）是最便宜的新旧二进制探针，无效先怀疑进程新旧而非代码；③多窗口/多进程用 EnumWindows 逐 HWND 归属 PID 再对号（FindWindowW 按类查跨窗口站/本环境多次失败，#55 同族）。
- **正确做法**：实测前显式 taskkill 残留（Get-Process 判）；每轮开工 PITFALLS #27 自查（StartTime 必须晚于最新 build）；"修复无效"三步裁决=进程新旧 → 二进制新旧 → 代码。
- **附带 PS 技巧**：EnumWindows 委托回调内 Write-Output/局部标志不外传——必须 ArrayList 闭包（引用类型）收集；复杂脚本落盘 .ps1 再 -File 执行（bash 内联 $ 变量会被 bash 吃掉）。

### 58. PrintWindow(flag 2) 黑边=DWM resize 边框伪影——判 UI 缺陷必须真屏对照 ⭐
- **症状**：PrintWindow 截图右缘 8px、左缘、底缘出现对称纯黑条，疑似控件渲染缺陷/裁切。
- **根因**：DWM 绘制的 resize 边框区域不属于应用绘制面，PrintWindow 对该区输出纯黑；真屏 CopyFromScreen 无此条。
- **正确做法**：截图取证优先 PrintWindow(hwnd, hdc, 2)（渲染窗口本体，抗遮挡窗口干扰），但**任何"疑似缺陷"必须真屏对照再定性**；黑边位置=窗口非客户区边框（对称、贴边）即伪影。判缺陷三件套：API 真值（GetScrollInfo 等）+ PrintWindow 取证 + 真屏对照。

- **症状**：设置窗"转发不弹窗"追了多轮：进程侧日志显示创建成功且 SW_SHOW，但 FindWindowW（按类名/标题）恒 0，一度判成"窗口被神秘销毁"的 P0。
- **根因**：agent 进程与交互桌面**同 Session 但不同 Window Station**。FindWindow 是"调用线程所在线窗口站"作用域——本会话窗口站里没有那个窗口，返回 0 是观测无效而非程序异常。最终用 Get-Process.Threads + EnumThreadWindows（按目标进程线程枚举）拿到真句柄，vis=True 实锤窗口一直正常显示，转发链从未断过。
- **正确做法**：观测其他会话/窗口站的窗口，用 EnumThreadWindows(目标进程线程) 或 UIA，别信 FindWindow；"查不到"先自问**我的眼睛在哪个房间**（#45 同族：跨会话观测假象的窗口站版）。转发/弹窗类问题以**进程侧日志（创建/复用打点）为第一证据**，窗口枚举为辅。


### 59. TSF InsertTextAtSelection 对"插入后的选区"零承诺——宽松测试宿主集体掩盖严格宿主光标停滞 ⭐⭐
- **症状**：用户真机报障"Win11 记事本上屏文字后光标停留原位，后续输入插到旧光标处"；全部自动化测试（deep/e2e/engine）全绿。
- **根因**：MSDN 对 `ITfInsertAtSelection::InsertTextAtSelection` 只承诺插入位置与返回 range，**选区是否随插入前进完全由宿主 store 实现决定**。灵犀 DLL 从不 `SetSelection`；而所有测试宿主（deep 旧版 TestStore/e2e/host32）插入后都"好心"自推选区——测试全绿掩盖了真机缺陷。真实宿主分两派：宽松（传统 EDIT 控件类、Chromium）自推；严格（Win11 记事本=WinUI CoreText TSF 适配层）不动 → 光标停滞。
- **修复范式**：上屏后显式 `Collapse(TF_ANCHOR_END)` + `SetSelection`（宽松宿主同位幂等、严格宿主补上前进）；测试宿主**有意分叉**建模两派——deep store 严格化（文本变更不动选区+GetSelection 钳制+宿主侧清空后重置选区），e2e 保持宽松，双覆盖。T19 光标契约用例（顺序+前缀+选区三元断言）常驻守卫。
- **严格化的连带必做**：store 不再自推后，**宿主侧操作必须自理选区**（ClearFocusedText 清空后重置 (0,0)，否则陈旧选区越界 → 下轮 `_buf.insert` out_of_range 崩溃）；插入点也要防御性夹取。
- **规约**：跨宿主 TSF 行为以"零承诺"为默认假设建模——凡规范不保证的，DLL 显式做掉，测试宿主按最严格宿主建模；宽松宿主作为第二风味覆盖。

### 60. 负载噪声窗口连杀 e2e 三 attempt + A/B/A 三样本陷阱——同源二进制"行为分叉"先证伪再排查 ⭐⭐
- **症状**：regress e2e 六连败（3 attempt × 2 轮），症状全同（pass-through 读空）；换 DLL(19:46 版)→PASS，换回(19:48 版)→FAIL，再换→PASS——"完美 A/B/A 反转"指向二进制差异；但安静环境复跑 19:48 版直接 attempt 1 绿——假红实锤为环境噪声窗口（19:47-58 机器负载期）。
- **根因 A（噪声连杀）**：负载窗口可持续数分钟，e2e 单轮 ~30s，3 attempt 全落在窗口内 → "重试吸收偶发"失效且症状同一，伪装成确定性缺陷。
- **根因 B（A/B/A 陷阱）**：三次交替实验里一次巧合即可制造反转假象；样本量 3 时"与自变量完美相关"不等于因果。
- **终结手法（本轮实证链）**：①`Get-FileHash` 判身份——`.prev` 轮转备份用 mtime+hash 定身份勿信文件名（build.cmd 的 PowerShell copy 保留源 mtime）；②逐字节 diff 两版二进制——本轮仅 7 字节且全在 PE 头/.rdata 时间戳区 → "功能等价却行为分叉"=物理矛盾 → 必有第三方变量（环境）；③安静环境复跑噪声期二进制是金标准裁决。
- **规约**：同源二进制（同 obj 重链，仅链接时间戳/GUID 差）出现"确定性"行为分叉时，先假设环境噪声并安静环境复现，再动代码；regress 失败先看时间窗（本机是否刚跑过重负载套件），不要直接回滚代码。
- **附带**：e2e 的 [DIAG] 诊断已挪到 fail-fast 判定之前（e2e.cpp），失败现场（store/focused/文档指针对照）从此永久可见。
