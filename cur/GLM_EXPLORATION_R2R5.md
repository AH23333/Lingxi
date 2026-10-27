# GLM_EXPLORATION — R2/R4/R5 预探索成果(替你省电,照做即可)

> QC 代理已完成探索;每条建议都落到 文件:行号,标注"已核实"。R3(LLM)另有专文。
> 行号基于当前 main 工作区(双拼提交后),若后续提交漂移请按符号搜索。

## R2 剩余:自学习用户词典的真缺口(已核实)

**唯一实质缺口:学习过的词永远进不了候选。**
- 现状链路:`Learn()`(pinyin.cpp)→ `_userDict[key]`(偏好表)→ `Predict` 里只做**重排**(pinyin.cpp:122 起的置顶段);
- 词的候选来源 `_entries`(主词索引)只在构造函数 `Load()`(pinyin.cpp:171→302)建一次;用户学的新词(如人名、网络词)不在基础词库时,重排无从发生——**"自学习"实际只是"已学词调频"**;
- `ngram.cpp` 不读用户词典(0 引用),英文侧无自学习(可不做,英文补全场景弱)。

**推荐方案:用户词入主索引(copy-on-write + 粗锁)**
1. `LoadUserDict()`(现只填 `_userDict`)扩展:对每个 (word,key) 用 `EnumeratePaths(key)`(pinyin.cpp:533,音节切分 DP,已核实可直接复用)切出音节 → 构造 `PyEntry`(rank 给专属值 `kUserRank`,排在普通词库命中之后、候选页末位)→ 走与 `Load()` 中条目入库相同的索引构建逻辑(插入点:`_entries.push_back` 所在的 Load 循环,pinyin.cpp:327 一带;索引构建代码建议从 Load 提出成 `IndexEntry(e)` 复用);
2. **并发雷点(必须照做,否则重蹈 `_userDict` 覆辙)**:`_entries/_byCompact/_byPattern/_byFirstSyll/_byFirstInit/_charBySyll` 目前只在构造期写、预测线程无锁读——安全。一旦 ReloadUserDict 要重建索引,UI 线程写 vs 预测线程读 = 新竞争。最简正确解:**把 `_udMtx` 粗化为全程锁**——`Predict` 整体持锁(Predict 中位 0.015~5.8ms,Learn/Reload 极低频,争用可忽略),`LoadUserDict`(含索引重建)与 `Learn` 同锁。单写者语义简单可靠;
3. 幂等:同 (word,key) 重复导入只更新 rank 计数,不重复建索引项(用 word→entry 指针的 side map 或重建时整表去重);
4. 大词库扩充:zh_pinyin.txt 已 30 万/6.2MB(读入内存无压力),**建议不再扩基础库**,把劲花在新词学习上;若确要扩,上限 50 万并实测 ctor Load 耗时(现在启动已有一次全量索引构建,注意冷启动变慢);
5. 繁体(st2t.txt,in-flight)与自学习的交互检查:Learn 存的是用户上屏词(繁体模式下是繁体)→ key 照存即可,但候选去重 seen 集合要避免简繁同词双份占页——提交前自测。

## R4 剩余:字体字号 / 皮肤 / 设置项(已核实插入点)

**字号**(半天工作量):
- `common/settings.h` 加 `uint32_t candidateFontSize = 16;`(注意现在有个风格笔误 `bool shuangpin =false;` 顺手修);
- `common/settings.cpp` Load/Save 各一行(照抄 CandidateCount 的范围钳位模式,建议 12..32);
- `ui/window.cpp:46/49/52` 三个硬编码字体高度(-16/34/-10)→ 按设置值 `MulDiv(-(int)fontSize, dpi, 96)`(dpi 已在窗口内可得,72 行有现成 MulDiv 用法);toast/小字按比例换算(×34/16 与 ×10/16)或也入设置;
- 即时生效:候选窗每次 `Show` 前对比缓存设置版本(读 LoadSettings µs 级,与 dark 跟随同模式),变了就重建三支字体并 InvalidateRect——注意 `DeleteObject` 旧字体(font cache 提交 4112e15 刚做过复用,别退化回泄漏);
- 设置界面:候选数量那排加一个"字号"ComboBox(16/18/20/24),照抄 CmbDark 模式。

**皮肤**:
- 配色集中在 `ui/window.cpp:240-281`(bg/fg/border/accent 常量,深浅已分支)→ 提取 `struct Skin{COLORREF bg,fg,border,accent,hlBg;}` 数组(预设 3-4 套:蓝/青/紫/暖),settings 加 `uint32_t skinIndex = 0;` 与可选 `accentColor`;改动面小,收尾做;
- 符号面板(settingsui 同款硬编码 RGB)若要跟随皮肤,把 Skin 传进去,优先级最低。

**隐身热键**:不建议新做——托盘"暂停预测"(Paused)已是等效功能,文档里把它改名/加说明为"隐身模式"即可,省一个全局热键独占(参考 Ctrl+Shift+B 的教训,好在你已做成可开关)。

## R5 工程化(已核实可行性)

**32 位 TIP(半天到一天)**:
- `ime/ImeCore.vcxproj:4-8` 只有 x64 配置 → 加 `Debug|Win32`+`Release|Win32`(照抄 x64 节改 PlatformToolset 输出目录为 `bin\$(Configuration)32` 或 `bin\Win32\$(Configuration)`);链接依赖全是系统库(ImeCore.vcxproj:52 已核,无 x64 专属依赖),编译预期零障碍;
- 注册:`C:\Windows\SysWOW64\regsvr32.exe ImeCore32.dll`——WOW64 重定向自动把 CLSID/CTF 写进 WOW6432Node 视图,与 x64 注册共存,**代码零改动**;unregister 两个 bitness 都要跑;
- **唯一代码坑**:`pipeclient.cpp:47` 的 `EnsureUIRunning` 从 DLL 自身目录拼 LingxiUI.exe——32 位 DLL 目录里没有 x64 LingxiUI → 回退链:自身目录 → `..\Debug\LingxiUI.exe` → 失败降级纯直通(现有降级路径已安全);
- 32 位宿主加载:系统按位数视图自动挑 DLL,无需手工;
- 验证:找一个 32 位应用(如老版 notepad?Win11 记事本是 64 位;可用 `C:\Windows\SysWOW64\notepad.exe` 测)打字看日志 `[pipe] 已连接`。

**卸载清理(2 小时)**:
- `tools/unregister.cmd` 增强(保持 ASCII):①两个 bitness 的 regsvr32 /u;②删 `HKCU\Software\LingxiIme`;③`taskkill /IM LingxiUI.exe`(铁律允许);④`--purge` 开关删 `%APPDATA%\LingxiIme`(含 userdict.txt,默认保留用户数据);⑤`del %TEMP%\lingxi-*`;⑥现有残留自检保留;
- 顺手:SignalNamedEvent 4 秒静默失败(main.cpp,遗留 P2)——失败时 MessageBoxW 提示"主实例未就绪"一次,别让用户点菜单无响应。

**稳定性(1-2 小时,零代码方案优先)**:
- 首选:WER LocalDumps 给 LingxiUI.exe 配置本机转储(管理员一次性):
  `reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\LingxiUI.exe" /v DumpFolder /t REG_EXPAND_SZ /d %%TEMP%%\lingxi-dumps /f` + DumpType=2 + DumpCount=5——崩溃自动落全量 dump,零代码;
- 代码方案(可选):LingxiUI `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`(dbghelp)写 `%TEMP%`;**DLL 侧禁止装 VEH/UEF**(宿主进程内装全局异常处理器=事故源),e2e 的 VEH 取证保留在测试内;
- 深度回归:`tools/regress.cmd`(ASCII):engine_test + e2e 连跑 3 轮,输出归档 `tmp\regress-<ts>\`,任一失败 exit 1——发布前一键;
- 文档收尾清单:README/HANDOFF/PROGRESS/PITFALLS 四件套同步(你有 Edge 结案通知待同步,见 cur/USER-CONFIRM-EDGE.md)。

## 建议执行顺序(按收益/风险比与电量)

1. **R2 新词入索引**(纯引擎,engine_test 可完整验证,不碰 TSF/不碰系统)——最高价值;
2. **R5 的 WER LocalDumps + regress.cmd**(1 小时,先给后续所有改动上保险);
3. **R4 字号**(半天,用户可感知);
4. **R3 LLM 重排** → **专文已出:cur/GLM_EXPLORATION_R3_LLM.md**(API 全部对 llama.h 行号核实、GGUF 头实测,含前置堵点:DLL 必须先补发上文);
5. R4 皮肤 → R5 32 位 → R5 卸载增强 → 文档收尾。

—— QC 代理,2026-10-28 01:3x
