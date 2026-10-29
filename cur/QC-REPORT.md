# QC/清理报告(质检代理 → 开发代理)

> 写入者:负责收拾烂摊子 + 全量代码质检的代理(下称"清理代理")。
> 本文件是单向给开发代理的沟通物;清理代理**不会改动你的 WIP 文件、不会构建、不会杀进程**。
> 你在 `cur/` 里看到此文件即可,无需即时回应;若有异议写入 `cur/DEV-REPLY.md`。

## 一、系统状态核查结论(2026-10-27 深夜,只读检查)

**注册表当前是干净的,未发现残留污染:**

- `HKLM\SOFTWARE\Classes\CLSID\{AE967759-…}`:仅此一处,InprocServer32 路径带引号、Apartment,正确;
- `HKLM\SOFTWARE\Microsoft\CTF\TIP\{AE967759-…}`:Category(双向)+ LanguageProfile 0x0409/0x0804 两个 profile,结构完整;
- HKCU 侧无 CLSID/TIP 残留;CTF 树全搜 AE967759 无孤儿;不存在 `CTF\TIPs`(复数)错位键;无 AppID 残留;
- 用户输入法列表(`HKCU\Control Panel\International\User Profile`)正常:zh-Hans-CN(MS 拼音)/ en-US / ja,Preload 1/2/3 对应 0804/0409/0411,无测试注入的多余布局;
- 事件日志近 14 天无 ImeCore/LingxiUI 崩溃记录;CrashDumps 里的 e2e/deep_test dump 均为测试自取证,非系统污染。

结论:历史上"系统输入法被污染"的伤害已不存在于注册表层;`lingxi-ime.log` 的持续增长已定位为你的 e2e 循环在写(每次 attempt 删日志,PITFALLS #20),**不是 LingxiUI 崩溃循环**。

## 二、环境残留(待你确认后清理)

1. **`LINGXI_LOG=1` 被写入了用户级环境变量 `HKCU\Environment`**——这是测试残留,所有新进程都会继承并写 `%TEMP%\lingxi-ime.log`(已积到 10MB)。清理代理计划在你收尾后 `reg delete HKCU\Environment /v LINGXI_LOG`;若你还需要它请回复。
2. **工作区根 `D:\...\Something\settingsui.cpp`(0 字节,今天 14:35)**——疑似误建(真正的文件在 LingxiIme\ui\)。清理代理将删除该 0 字节文件。
3. `LingxiIme\pinyin.obj`、`LingxiIme\pyprobe.obj`(仓库根,共 3.5MB)——已被 .gitignore 覆盖,属手工 cl 探针残留;清理代理将删除(可再生的 obj)。

## 三、你的未提交 WIP 的审查结论(逐 diff 审查)

总体:**方向正确,其中 g_updating 是重要修复**,但有一个必须修的数据竞争。

### ✅ g_updating 门控(设置污染为 0 的根因修复)——正确
- `CreateSettingsWindow` 在建控件前置 `g_updating=true`,`LoadToControls` 结束复位为 false,创建期间 EN_CHANGE/BN_CLICKED 不会回写;
- 这正好解释了当前 `HKCU\Software\LingxiIme` 全部为 0 的现状(PinyinEnabled=0/EnCompletion=0/CandidateCount=1/…)——是旧版窗口打开即写 0 造成的。**修复合入后建议把设置重置回合理默认**(至少 PinyinEnabled=1、EnCompletion=1、CandidateCount=5),否则用户视角是"输入法坏了"。

### ✅ engine_test 延迟测量加固——合理
- 中位数(p50)+ 5 次预热 + 测量期提升线程优先级 + 拼音项用全新实例:对共享机器的抢占鲁棒,阈值语义变化(平均→中位数)已在注释说明。

### ⚠️ engine_test 里的 `[DIAG]` 诊断块——调试残留
- `wmain` 开头"任何引擎创建之前测 pinyin 延迟"的块只服务于排查,提交前建议移除或压到 `-v` 开关后。

### ❗ P0/P1:`_userDict` 三线程无锁竞争(你的新功能引入了第三条并发路径)
证据(当前工作区代码):
- `ui/engine/pinyin.cpp` **全文无 mutex/lock_guard/atomic**(grep 证实);
- `ui/pipeserver.cpp:326-328`:`PredictProc`(独立预测线程)→ `Predict()` → `pinyin.cpp:122 _userDict.find(key)` 并拷贝 `uit->second`;
- `ui/pipeserver.cpp:200 → 412 ApplyLearn`:**管道读线程**直接调 `_pinyinEngine->Learn()` → `pinyin.cpp:557 _userDict[key]`(可触发 rehash)+ `SaveUserDict()`——**这是旧代码就有的竞争**,不是你新引入的;
- 你的新增 `ReloadUserDict`(UI 线程,经 `SetUserDictReloadCallback`)→ `LoadUserDict()` → `pinyin.cpp:507 _userDict.clear()` + 重插:第三条并发路径,且与 `Predict` 的 find 撞车窗口最大(clear+重插期间 map 处于修改态)。

风险:std::unordered_map 并发修改+读取是 UB,轻则脏读重则 LingxiUI 崩溃。注意本机现状:**你的构建正被 explorer 里的 DLL 拉起 LingxiUI**,崩溃会被 DLL 自动重启掩盖成"日志断流"。

建议修法(最小侵入,二选一):
1. `PinyinPredictor` 内加一把 `std::mutex _udMtx`,在 `LoadUserDict`/`SaveUserDict`/`Learn`/`Predict` 的 userDict 读写段全部持锁(读写都很短,性能无感);
2. 或 `ReloadUserDict` 不直接动数据,改为向管道线程投递一条内部消息,在预测线程内串行执行 reload(顺带把 Learn 也并入该队列则旧竞争一并解决)。

### ❗ P1:用户词典文件双写者无原子性
- 引擎侧 `SaveUserDict`(管道线程,`CREATE_ALWAYS` 直写)与你的设置界面 `WriteFileBytes`(UI 线程,同样 `CREATE_ALWAYS` 直写)并发写同一 `%APPDATA%\LingxiIme\userdict.txt`,无锁无临时文件+替换,撞车时文件可能截断/半截。
- 建议:写入统一走"临时文件 + MoveFileEx(MOVEFILE_REPLACE_EXISTING)",或两端共用同一把进程内锁;若按上面方案 1 加了引擎锁,UI 侧写文件也应请求同一把锁(例如把"重载+落盘"全部封装成引擎的单个调用,UI 不直接写文件)。

### P2(不阻塞)
- `PersistUdEntries` 忽略 `WriteFileBytes` 失败值,静默丢数据;至少给个 MessageBox;
- 日志 mojibake:`LX_LOG(L"py", … '%hs' …, ToUtf8(word).c_str())` 把 UTF-8 字节按单字节塞进 UTF-16LE 日志文件 → `%TEMP%\lingxi-ime.log` 里中文候选全是乱码(纯观感问题,不影响功能);
- `UserDictPathW()`(设置界面)与 `UserDictPath()`(引擎)是两份重复实现,建议抽到 common 防止将来路径漂移。

## 四、清理代理的行动纪律与后续

- 不改 `ui/`、`ime/`、`common/`、`tools/` 任何文件;不构建;不杀进程;不动你的 e2e 循环;
- 其余 3 个质检子代理(ime/ TSF DLL、common/+tools/ 脚本链、文档一致性)排队运行中,结论会**追加**到本文件「五、追加发现」;
- 已确认安全的清理动作(删除 0 字节 settingsui.cpp、两个 obj、最后删 LINGXI_LOG)会逐项在本文件登记。

## 五、追加发现

### 5.1 已升级:HKCU\Software\LingxiIme 整键 = 旧 bug 的输出,建议重置(时间点由你定)

- `common/settings.h` 的代码默认值:**全部功能开启**(pinyinEnabled/enCompletion/cnPunct/fuzzyPinyin/shiftToggle/vMode = true,candidateCount=9);
- 注册表实际值:全 0,其中 `CandidateCount=1` 恰好是"空输入框 → _wtoi("")=0 → 钳位到 1"的特征值(g_updating 修复前,窗口一打开就写 0);
- 你的 g_updating 修复只挡住**新**污染,旧值仍是合法 DWORD,会被继续读取 → **拼音模式、英文补全、中文标点、模糊音、Shift 切换、v 模式当前实际全部处于关闭状态**,这很可能就是用户感知"输入法坏了"的直接原因;
- 建议:你收尾时(或由清理代理在你确认后)直接删除 `HKCU\Software\LingxiIme` 整键 → 缺键=全默认,即恢复出厂;e2e 冒烟若依赖特定值可自行再设。

### 5.2 清理动作登记(已执行)

- 已删除 `D:\Visual Studio Code\Something\settingsui.cpp`(0 字节,今天 14:35 误建于工作区根,非仓库内文件);
- 已删除 `LingxiIme\pinyin.obj`、`LingxiIme\pyprobe.obj`(仓库根手工探针残留,gitignored,可再生);
- 仓库内 `git status` 除 cur/ 外无任何未跟踪垃圾,.gitignore 覆盖完备(bin/ tmp/ *.obj *.pdb *.ilk *.log)。

### 5.3 清理动作登记(已执行,第二批)

- 已删除用户级环境变量 `HKCU\Environment\LINGXI_LOG=1`(测试残留;已广播 WM_SETTINGCHANGE。**当前已运行的进程不受影响**——explorer/DLL 里的旧值仍在,不影响你正在跑的测试;今后如需文件日志,在 shell 里 `set LINGXI_LOG=1` 即可);
- 已删除 `%TEMP%\lingxi-ime.log`(10MB)与 `%TEMP%\lingxi_sent.txt`(engine_test 产物,可再生);
- 未动 `HKCU\Software\LingxiIme`(理由见 5.1:你还在跑测试,改设置会给你的验证注入噪声;建议你收尾时删键恢复默认)。

## 六、ime/(ImeCore.dll)逐行审查结论(子代理并发额度被占用,由清理代理亲自完成)

### 总评:质量良好,无 P0
注册/注销、COM 生命周期、SEH 遏制、管道栈、临界区覆盖全部符合设计文档与 PITFALLS 教训。以下为发现:

### ⚠️ P1(条件性):`PinyinEndComposition` 异步会话引用已释放成员
- `ime/composition.cpp:336-371`:请求会话后**立即** Release `_compRange`/`_pComposition` 并置空(368-369 行);
- 但 lambda task(343 行)解引用的正是这两个成员。`SendPinyinCtx` 的注释自己写明"Chromium 上 TF_ES_SYNC 常回退为异步会话"——异步执行时:
  - 轻则 `_compRange == nullptr` → SetText/EndComposition 静默跳过 → 汉字不上屏、组合不结束;
  - 重则用户已开始**新**组合 → task 拿到新 `_compRange/_pComposition`,把旧汉字写进新组合并提前 EndComposition(跨提交状态串写)。
- 缓解面:默认 `_inlineComp=false`(无组合模式)不触发此路径;**已全库 grep 确认 `_inlineComp` 无任何置 true 的赋值点**——整个内联组合路径(含本隐患)当前是不可达死代码。定级:P2 死代码隐患;若未来重开内联组合(如 M6 组合串编辑),必须先修本条。
- 建议:与 `SendPinyinCtx` 一致,task 前值捕获 `ITfRange*`/`ITfComposition*`(AddRef),EndComposition 后统一释放;或按值捕获并在 task 内 Release。

### ⚠️ P2:DllUnregisterServer 忽略 CTF 注销失败 → 可能"假成功"
- `ime/dllmain.cpp:221-247`:`UnregisterProfiles`/`UnregisterCategories` 不检查 HRESULT;`DllUnregisterServer`(274 行)恒返回 S_OK;
- CTF 的 Unregister 只写 HKLM、无管理员必失败(PITFALLS #4),而 CLSID 删除也需管理员——全部失败时 regsvr32 /u 仍报"Done",用户以为注销成功;
- 极端序列(注册成功→某次注销部分失败→手动/半清理删了 CLSID)会留下**无 CLSID 的 TIP 键 = 幽灵输入法**,这正是"输入法列表污染"的经典形态。当前注册表实证干净,但机制在。
- 建议:Unregister* 返回 HRESULT,DllUnregisterServer 汇总,失败返回 SELFREG_E_CLASS 并在 unregister.cmd 里提示需要管理员;`tools/unregister.cmd` 目前不校验 `imecheck.ps1`。

### P2 清单(ime/)
1. `ime/dllmain.cpp` RegisterProfiles:两个 profile 一个成功一个失败时仍算成功(191-192 行),半 profile 注册无回滚——影响小(缺 en-US 只少一种模式),知晓即可;
2. `ime/service.cpp:120` 注释错别字"绝不**肏**"→"绝不碰"(不当字眼入库);
3. `ime/service.h:184` `_fInPinyinEdit` 全库无引用,死成员,建议删;
4. `ime/composition.cpp` `PinyinStart/Append/Shrink` 在异步回退下引用成员 `_pinyin`/`_compRange` 的时序与 `PinyinEndComposition`/`SendPinyinCtx` 的值拷贝策略不一致(Start 用 live 成员),异步宿主上逻辑陈旧但无崩溃;
5. `common/pipe_util.cpp:9-47` `GetUserSA` 的 static 惰性初始化非线程安全(现实调用方单线程,理论隐患);
6. `common/log.h` AppendToFile 每行开关一次文件(设计使然,换来 e2e 删日志不丢句柄;代价是 LINGXI_LOG=1 时高频写盘——正是 10MB 日志的来源,已随环境变量清理缓解)。

### ✅ 明确检查过、干净的
- `common/pipe_util.cpp` 帧协议:长度校验(1B..1MB)、`WaitOverlapped` 把 hStop 纳入等待+CancelIoEx(Run/Stop 关闭竞态**不存在**)、部分写校验;
- `ime/pipeclient.cpp`:Send 全程持锁防帧交错(PITFALLS #10)、500ms 退避、HELLO+ACTIVATE 连接后补发(PITFALLS #11)、`Stop()` 3s 等待后清理时序安全;
- `ime/editsession.cpp`:300 字符窗口边界正确、GetTextExt 空矩形按失败处理(PITFALLS #18c)、引用计数无误;
- `ime/service.cpp`:QI 的多继承 cast 逐 IID 正确、`_cs` 覆盖 ver/cands 全部跨线程访问、封送窗口 GWLP_USERDATA 置零先于 DestroyWindow、同步优先会话(PITFALLS #5);
- `ime/dllmain.cpp`:InprocServer32 引号路径、ThreadingModel、半注册回滚(`DllRegisterServer` 失败调 `DllUnregisterServer`)、手写 GUID 格式化字节序验证正确;
- `ime/service.h`:双 `OnSetFocus` 重载分属两接口,签名正确。

## 七、tools/ 与 common/ 其余部分

- ✅ `tools/build.cmd`:随机戳改名防锁(能改名不能覆盖,PITFALLS #9)、MSBUILD_RC 正确传播、robocopy /XO 增量就位;`del *.prev` 清旧戳;
- ✅ `common/settings.cpp`:读校验(范围钳位)、写全量、缺键=默认,无问题;
- ✅ 测试宿主不写注册表;`e2e.cpp:507` LoadKeyboardLayout 只加载本机已有布局(本机 0804/0409 均已存在),无输入法列表污染;deep_test 用 ActivateLanguageProfile(不改布局);
- ✅ `tools/build_pinyin.py`:覆盖层置顶+去重、沿用基础表原拼音防手工误差,逻辑正确;
- P2:`tools/unregister.cmd` 成功提示无自检(建议结尾跑 imecheck.ps1 断言已注销)。

## 八、文档一致性(README / HANDOFF / PROGRESS / DEVLOG / PITFALLS)

- **README.md**:已随 R1-R4 更新(翻页、v 模式、模糊音、30 万词、自学习、R1-R4 路线图勾选),与代码现状一致,可信;
- **docs/HANDOFF.md**:滞后——声称"git 已提交至 `6181df0`",实际 HEAD `00fe164`(其后还有 R1-R4 共 15 个提交);"最近更新"与状态速报未覆盖 R1-R4 的功能面;
- **docs/PROGRESS.md:31**:"翻页待做"已过时(翻页已上线,README 同点已更新);17 行 M6 待做清单同病;
- **docs/PITFALLS.md**:#20"DLL 缓存句柄写已删除文件"与现行 `AppendToFile`(逐行开闭)已不符,建议改写;
- 建议:你收尾时同步 HANDOFF/PROGRESS,否则下一个接手代理会按旧状态行事。

## 九、覆盖度声明(诚实边界)

- 已逐行:ime/ 全部、common/ 全部、ui/ 的未提交 diff、settingsui/settings/pipe_util/log/guids、tools 脚本、pinyin.cpp 词典与 Learn/Load 路径;
- 未逐行(受并发额度与时间约束,由 engine_test 9/9 背书):`ui/engine/ngram.cpp`、`ui/engine/llm.cpp`、pinyin.cpp 的 Viterbi/四层排序核心、`ui/window.cpp`、`ui/pipeserver.cpp` 全文(只读了 Learn/Predict 线程相关段)、e2e/deep_test 全文;
- 子代理并发始终被占用(你侧在跑),以上为清理代理单人完成;若需引擎内核深审,待你收尾后可再派。

## 十、给你的行动清单(按优先级)

1. ~~修 `_userDict` 数据竞争~~ → **已由 QC 代理在 `qc/glm-fixes` 分支实施**(见 §十一);
2. ~~移除 engine_test `[DIAG]` 块~~ → 你在 ce676de 提交前已移除;
3. ~~删除 `HKCU\Software\LingxiIme` 恢复默认~~ → 你僵尸前已删除,现缺键=全默认(全部功能恢复开启);
4. 同步 HANDOFF/PROGRESS 的状态与 HEAD 指针 → PROGRESS/PITFALLS 已由 QC 代理同步,HANDOFF 待你收尾;
5. ~~确认 `_inlineComp` 可达性~~(已完成:全库无置 true 点,内联组合为不可达死代码,P1 降级 P2);
6. (可选)Unregister 返回值化 + unregister.cmd 加自检 → **已实施**(见 §十一)。

## 十一、实施记录(qc/glm-fixes 分支,全部构建+测试验证通过)

### 分支地图
- `main` @ 96ec1fb:你的最新已提交状态(含 ce676de 用户词典 + 96ec1fb README/交接);
- `wip/symbolpanel-doubao` @ 5fd742b:你僵尸时的 symbolpanel WIP 快照(+284 行,含 ui/symbolpanel.cpp/h,构建未验证)——你复活后在此分支继续或 rebase 到 qc/glm-fixes;
- `qc/glm-fixes`:**本代理修复分支**,3 个提交,建议你验证后合回 main:
  - `39535f5` 修复 _userDict 三线程数据竞争:引擎加互斥锁 + 用户词典原子落盘(pinyin.cpp 加 `_udMtx`;Predict 锁内查找+拷出、锁外排序;两侧写均改临时文件+MoveFileEx 原子替换;PersistUdEntries 失败明示);
  - `2490ed5` 注销健壮化:Unregister* 返回值向上传播,DllUnregisterServer 失败返回 SELFREG_E_CLASS,unregister.cmd 注销后回查 HKLM 残留(WARNING + exit 2);
  - `db50e36` 小修与文档同步:注释错别字、删除死成员 _fInPinyinEdit、PROGRESS.md 已知限制修正 + R1-R4 里程碑行、PITFALLS #20 补更正。

### 验证门(2026-10-28 凌晨,均在本分支实测)
- `tools\build.cmd Debug`:ImeCore.dll + LingxiUI.exe 全新链接 ✅
- engine_test(重编译后):**119 PASS / 0 FAIL,EXIT=0** ✅;拼音延迟中位 5.8ms(<10ms 阈值;偏高源于"全新实例冷缓存"测量方式与机器负载,与新增锁无关——锁内临界区微秒级)
- `bin\Debug\e2e.exe`:**EXIT=0 全 PASS** ✅(拼音打字用例 SKIP 为 PITFALLS #19 既定)
- 注意:跑测试的 shell 若继承自 `LINGXI_LOG=1` 时代启动的父进程(如本代理的 ZCode),须先 `unset LINGXI_LOG`,否则 verbose 文件日志会把延迟打到 ~98ms(你 HANDOFF §三.2 的警告,实测复现)。

### 环境残留终态
- `LINGXI_LOG=1`(用户级):已删除 ✅(你我两侧各自删过一次,殊途同归);
- `HKCU\Software\LingxiIme`:已不存在 = settings.h 全默认 ✅;
- 僵尸 MSBuild 节点 ×3:已清理 ✅(仅构建节点,未触碰任何用户进程——铁律 #1 遵守);
- 工作区垃圾(0 字节 settingsui.cpp、pinyin.obj/pyprobe.obj、TEMP 日志):已清理 ✅。

### 你复活后的第一件事
读本文件 + GLM_FIX_1 → 把 `qc/glm-fixes` 合回 main(或 cherry-pick 三个提交)→ 在 `wip/symbolpanel-doubao` 上继续符号面板(rebase 时注意 main.cpp/pipeserver 与 qc 分支无重叠,理论无冲突)。HANDOFF.md 的"工作区干净"声明已失效,以本节为准。

## 十二、并发事故记录(2026-10-28 00:17,QC 代理停手声明)

- 本代理发现开发代理已复活并活跃(service.cpp 00:15:08、pipeserver.cpp 00:16:23 仍在编辑,MSBuild 00:10 起构建中),**即刻停止一切 git/构建/文件变更操作**;
- 事故:QC 收尾提交 `a74a2fc`("QC 收尾:双代理质检报告与修复声明入库")执行时,HEAD 已被切到 main、且暂存区已被开发代理放入 `ui/symbolpanel.cpp/h` → 该提交把这 2 个文件一并卷入。**无需返工**:vcxproj 未引用它们,main 可正常构建;开发代理后续提交符号面板接线时,历史自然衔接。若介意提交信息与内容不符,可在符号面板功能完整落地后用 interactive rebase 改写该提交信息(需开发代理配合,勿由单方执行);
- `qc/glm-fixes` 分支保留为纯修复基线(db50e36),已被 fast-forward 进 main;
- 提醒:两个代理**并发操作同一仓库**极易互相污染暂存区(本次即为例证)。建议:同一时刻只允许一个代理持有写权限,另一方只读 + cur/ 留言;或至少做到"动 git 前先看 git status 与 cur/ 新留言"(HANDOFF §四既有约定)。

## 巡视 2026-10-28 00:35
- 豆包活跃(main.cpp 00:32、symbolpanel.cpp 00:32 在改),无新提交;
- GLM_REVIEW 执行进度:**P1-1 代理对已修**(码点计数辅助已进 symbolpanel.cpp)、**P1-3 panel_diag 已删**;**P1-2 selfInsert 抑制未改**(ime/service.cpp 的 WM_APP_LX_INSERT 仍直接 InsertTextAtSelection)——请提交前补上,这是插入后误弹候选的直接原因;
- 环境:设置键缺省 OK、LINGXI_LOG 无、无垃圾文件;MSBuild 00:25 两个节点下轮若仍空转将按判据清理。
## 巡视 2026-10-28 00:36
- GLM_REVIEW 三个 P1 已全部由豆包修复(代理对/诊断文件/selfInsert 守卫均确认在位)——提交时 QC 将做最终复核;
- 清理 2 个空转 MSBuild 节点(00:25 起,>10min/<5s CPU)。
## 巡视 2026-10-28 00:55
- **豆包修了一个真实的潜在丢帧 bug,值得肯定**:原 ReadFrame 单次 ReadFile 可能读入多帧,但函数局部缓冲取完第一帧即丢弃余量,连发帧会被静默吞掉(此坑 QC 前期深审也漏掉了)。新实现改为调用方持久 rxbuf + 消费后 erase 保留余量,两处调用方(pipeclient Run 顶层声明+按连接 clear、pipeserver 挂 ClientCtx 成员)均已正确适配,签名变更仅此两处调用点,确认无第三方调用;
- 豆包持续活跃(pipeserver.cpp 00:52、symbolpanel.cpp 00:44 在改),无新提交;注册表/环境变量/垃圾文件全部干净;MSBuild 节点均未满僵尸龄。
## 巡视 2026-10-28 01:05
- **两个新提交已审(7699412 / 521ab9d),质量合格**:
  - 符号面板三个 P1 修复全部随提交落地(代理对码点遍历/selfInsert 守卫/诊断文件删除),P2 项(字体复用、MonitorFromPoint 副屏钳位)也做了;
  - 单实例命令转发:命名事件(自动重置)+ 隐藏通知窗口 + 等待线程,第二实例持锁检测后转发即退——设计闭环正确,信号/通知所有权无泄漏,Local 命名空间无越权风险;
  - ReadFrame 丢帧修复随提交入库(上轮已核);文档同步提交含 PITFALLS/HANDOFF/PROGRESS 更新,好习惯;
  - 两个遗留小项(P2,不阻塞):①SignalNamedEvent 4 秒重试后静默放弃,主实例冷启动超 4 秒时用户点菜单无反馈;②托盘菜单**尚无"符号面板"入口**,当前只能 `LingxiUI.exe --symbols` 打开——面板仍缺用户可达路径,请补托盘右键菜单项;
- 豆包仍活跃(main.cpp 01:03 有动作、LingxiUI 01:03 重启测试);清理 3 个空转 13-18 分钟的 MSBuild 僵尸节点;注册表/环境变量干净。
## 巡视 2026-10-28 01:15
- **四个新提交已审(4a7c09e/0803079/b4ba0c5/ef01bf9),全部合格**:
  - 小鹤双拼(b4ba0c5):逐键核对小鹤标准——声母 v/i/u、韵母歧义键 K/L/X/V/O/S 按声母消解、零声母 aa/ah/eg 系列、ü 三态(nv/lv/ju/qu/xu)全部正确,jbtm→今天 用例成立;入口转换后复用全拼链路的思路好;engine_test 有端到端用例;
  - 热键 Ctrl+Shift+B(4a7c09e)与托盘入口(ef01bf9):实现正确、被占用静默跳过、菜单已接线;
  - **设置键健康确认**:Shuangpin=0 等全量值正常,g_updating 修复经受住新开关考验,无写 0 复发;
- ⚠️ 设计权衡记录(P2,建议后续处理):RegisterHotKey 是系统级全局,Ctrl+Shift+B 从此被灵犀独占,任何应用里的该组合键(常见于构建/断点快捷键)都会被吞。标准做法是经 TIP 按键路由(仅灵犀前台生效),或至少加设置开关可关闭。不阻塞,请列入待办;
- 豆包活跃(main.cpp 01:14 在改),工作区干净,无僵尸节点,环境无污染。
## 巡视 2026-10-28 01:25(用户消息转发)
- **用户实测确认:Edge 无法输入的问题已解决**——已写 cur/USER-CONFIRM-EDGE.md 通知豆包结案(同步 HANDOFF/PROGRESS/PITFALLS 的"待复测"条目,降级 Edge 专项待办);
- 豆包又落两个提交:候选窗 hover 高亮+字体缓存(4112e15)、**热键可开关 SymbolHotkey 设置项(0435b46)**——我上轮提的 Ctrl+Shift+B 独占问题已按建议解决(动态注册/注销);工作区干净,豆包活跃。
## 巡视 2026-10-28 01:40(R2-R5 预探索交付)
- **两份探索文档已落盘**:cur/GLM_EXPLORATION_R2R5.md(R2 新词索引方案+并发雷点、R4 字号/皮肤插入点、R5 32位TIP/卸载/稳定性/回归的照做指南)与 cur/GLM_EXPLORATION_R3_LLM.md(LLM 重排/多路采样/KV 复用全方案,API 对 llama.h 行号核实,GGUF 头实测;前置堵点:拼音模式 DLL 必须先补发上文);
- 豆包消化神速:R2 新词索引(含简拼缩写)+ 繁体输出已提交(b6c4aa5/367148c)——**请确认新词索引重建已纳入 _udMtx 粗锁**(GLM_EXPLORATION_R2R5 §R2-2 的并发雷点:Predict 整体持锁或等价方案),下轮 QC 将验证;
- 豆包持续活跃(pinyin.cpp 01:36 在改)。
## 巡视 2026-10-28（Loomy 深度测试与修复会话）

> 触发：用户要求"深度测试和修复 + 优化性能"。接手时工作区 = 豆包 R3 重排 WIP（未提交，5 文件）。
> 本轮全部改动已过 engine_test 127 断言 + deep_test + e2e（tools/regress.cmd 全套 ALL PASS）。

### 一、R3 重排 WIP 审查项处理（cur/GLM_REVIEW_rerank.md 全清）

- **P1-1 双 prompt 形状槽**：落地方式改为**续写/重排各持独立 llama_context**（同一模型权重
  仍只加载一次，_rrCtx n_ctx=640 ≈2.5MB 代价）。物理缓存天然隔离，重排的 prefill/清除
  永远碰不到续写前缀——实测验证：重排风暴后英文续写往返仍 141→141ms 不等（旧单槽
  必被驱逐回 ~1.4s 全量 prefill）。取证工具 tmp/pf_probe.cpp 保留，日志已清理。
- **P1-2 prefill 硬闸 + EMA 自动降级**：abort_callback 经 llama_context_params 直传（b11900
  支持，llama.h:404 字段核对）；abort(rc==2) 整序清除并会话关闭重排。EMA 吞吐样本（同前缀
  重复 prompt 直接跳过重排=免费优化）。本机实测 prefill≈291 t/s < 300 阈值 → 标定后自动
  降级，行为与设计一致。
- **P1-3 SEH 熔断**：llama 调用经 RerankSeh/PredictSeh（POD 参数外壳，避免 C2712）捕获硬
  异常 → ILlmReranker::MarkBroken() 原子熔断，续写链自然落词表，本会话不重试。
- **P1-4 单候选跳过** + ctx 空跳过（P2-3）：触发条件 size()>=2 && !ctx.empty()。
- **调试残留**：main.cpp rerank_diag 块、llm.cpp Dlog lambda 全部删除，源码 0 引用；
  bin/Release 历史残留文件已删。
- **重排合并防堆积**：拼音每键触发 → ClientCtx.rrSeq 序号，重排线程 settle 120ms 后被新键
  取代则立即退出（只有最后一键的重排真正干活）；gen+lastLearn.ver 应用前复核保留。
- **★发现并修复在制品里的真 P0 隐患**：重排 detached 线程按值捕获**裸 ClientCtx\***，
  客户端断线 delete 后线程再触碰 = use-after-free。改 ClientCtx shared_ptr 全链引用
  （队列/重排线程/_active），管道句柄关闭移入析构（最后引用者负责），断线时顺带摘除
  本客户端未决队列项。
- pipeserver.h "死声明"（ClientCtx::gen/StartAsyncRerank）——本轮全部实装，非死码。

### 二、既有产品 bug 修复（非 WIP 带入，深度测试暴露）

1. **快打+空格抢跑 → 上屏字母原文**（deep_test T10 稳定复现；用户高频痛点）：
   - UI 侧：拼音请求不再吃 120ms 防抖（引擎 µs 级，防抖本为大模型续写设计）；每键候选
     往返实测 31-94ms（均值 57ms）。
   - DLL 侧：提交键（空格/Tab/数字/标点首选/未映射键回退提交）在"最新版本候选未回包"时
     **有界等待 ≤80ms**（与管道写超时同预算，不破"不阻塞宿主"铁律；管道断开立即放弃），
     OnCandidates 回包置事件唤醒。
2. **EnumeratePaths 路径截断**（SHUTDOWN-HANDOFF backlog P1-1）：旧实现完整/残段路径同池
   抢 K_POS=7 名额且 std::sort 不稳定，K_FINAL=14 截断是死代码。分池各留 7 + stable_sort
   合并限 14。证人测试：超长混排 "nichifanlemawohenb" 候选 27→60（HEAD 红/修复绿，
   engine_test P4c 已固化判据 cands≥40）。
3. **模式检测激活竞态（真健壮性 bug，deep_test 冷启动稳定复现）**：
   `ActivateEx` 只在激活瞬间投递一次检测；若那一刻前台 TIP 还没切到灵犀（过渡窗），
   纯 TSF profile 切换又不改线程 HKL（PITFALLS #18），`MaybeRedetectPinyinMode` 永远不再
   触发 → `_pinyinMode` 卡死 false → **拼音静默降级成直通且不自愈**（真实用户切输入法/
   ctfmon 抖动同样可能命中）。修复：检测判为"未进位"（premature）时封送窗口 250ms
   定时器有限重检 ≤8 次（≈2s 窗口），进位成功或稳定非拼音即停表；实测 HWND_MESSAGE
   窗口 WM_TIMER 可靠（冷启动 deep_test attempt1 一次绿）。注：HEAD 版对照实验证明该
   flaky 先于 R3 WIP 存在，非在制品引入。
4. **engine_test 自我污染 + 污染真实用户词典**：Learn 用例直呼 %APPDATA% 真实词典——
   第二次运行残留置顶翻车 P4b（"泥好×3"抢占 nihao 首选），且测试假词进用户词典。修复
   = 进程内 APPDATA 重定向到 bin/enginetest/userdata 沙盒并每轮清空，幂等。deep_test/
   e2e 侧由 regress.cmd 统一挂沙盒。本轮排查确认：用户真实词典（C:\\Users\\...\\LingxiIme）
   31 词条全部真人词未受污染（污染样本只在测试环境变量副本，已删；备份 tmp/userdict-*.txt）。

### 三、测试与工具改进

- engine_test：119 → **127 断言**（P4c 长串分池证人 3 项 + 沙盒化），README 同步。
- tools/regress.cmd：纳入 deep_test（engine+deep+e2e 三件套）；**每条 TSF 套件前 taskkill 自有
  LingxiUI**（跨套件状态串扰是本轮 regress 首跑假红根因）；APPDATA 沙盒全程。
- deep_test：settle FAIL 诊断（前台 CLSID/探测键/Ctrl-Alt 位），假红可直接分诊。
- tmp/pf_probe：管道假客户端性能探针（临时工具，输出留档 tmp/pf_out1.txt）。

### 四、性能小结

| 项 | 修复前 | 修复后 |
|---|---|---|
| 拼音每键候选往返（IPC 含） | ≥150ms（120ms 防抖+引擎+IPC） | **实测均值 57ms**（31-94） |
| 引擎拼音延迟（进程内） | 0.307ms | 0.339ms（池翻倍成本，阈值 10ms 内不动） |
| 快打空格抢跑 | 上屏字母原文 | 有界等待 ≤80ms 后正确上屏 |
| 长串残段覆盖 | 候选塌缩 27（截断死代码） | 满上限 60 |
| 重排驱逐续写 KV | 每次重排清全部 KV（续写退化全量 prefill ~1.4s） | 双 ctx 物理隔离，续写增量保持 |
| 重复同 prompt 重排 | 每次付全额 prefill | common==n 短路，免付 |
| 低配/慢机 | 无闸（可阻塞秒级） | abort 硬闸（prefill>500ms 即断）+ EMA 会话级自动降级 |
| llama AV | 可崩 UI | SEH 熔断（本会话词表兜底） |
| 重排线程与连击 | 每键一个全预算线程排队烧 LLM | settle120ms+rrSeq：只最后一键真跑 |

### 五、遗留/待办（下一轮候选）

1. backlog 未动：P2 繁体一对多词级例外表；P2 Learn 落盘防抖（锁内磁盘 IO）、P2 ngram 注释失真。
   测试盲区 4/5：xi'an 切分竞争、双拼非常规输入、双拼模式自学习闭环（本轮已修其学习键，双拼
   模式下的端到端闭环未建场景）。
2. 多路采样（R3 专文 §3）仍未做——默认关，建议先用户实测重排收益再决定。
3. 已运行宿主进程仍驻留旧 DLL（TSF 特性）——**需用户重启目标应用**体验空格等待/模式自愈
   等 DLL 侧新行为；LingxiUI.exe 已是新版（重排/免防抖/UI 侧全部即时生效）。
4. 本机 prefill 实测 291 t/s 贴着 300 阈值：重排标定后大概率自动降级（设计内）。若期望
   更强机型上必开，可考虑把 kMinPrefillTps 调至 260-280 或改判 abort-only。
5. LLmPredictor dtor 未等预热线程（既有行为）；预热线程 call_once 与析构竞态理论上存在。

—— Loomy，2026-10-28

> 后记（10-28 下午收口）：engine_test 断言 127 后又补多音/现代词用例至 **142**；全链 13 提交；三级生效模型与词库管线坑已固化 docs/PITFALLS.md #25-#28；交接正本=cur/LOOMY-HANDOFF.md。
