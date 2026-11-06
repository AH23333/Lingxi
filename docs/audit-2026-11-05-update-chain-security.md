# 深度审查卷宗：更新链/提权面/引擎交互（2026-11-05 起，滚动追加）

> 方法与纪律：账面只作线索，一切结论出自审查会话亲手执行的盘上实测（脚本输出/现读字节）；
> 每轮含"当轮判卷+勘误表"——自我推翻与推翻注入噪声同权记录，不留沉默。
> 本卷宗**只审查不修改代码**；修复建议标注归属（安全/主线）但不代拍。

---

## 轮 1 · 防线存在性扫描（12 攻击面）

**正面确证（全部有代码现场）**：
- fetch 侧：HTTPS 域名白名单+逐跳复核、REDIRECT_POLICY_NEVER 手动逐跳、zip 条目**字符白名单**（自然排除 ADS/盘符/反斜杠）、zip64 EOCD 哨兵拒、解压炸弹双上限（单 64MB/总 256MB）+解出后逐文件实测复核+120s 硬超时、SUMS↔zip 绑定校验。
- apply 侧：T10 staging 约束（binRoot 从**自身模块路径**推导不采信 argv；目录名=合法 SemVer；`_wcsicmp` 父目录硬比对）、T9 rename-aside 两阶段+整体回滚、pending-rollback 标记只记未完成项+**启动自检接线真实存在**（ui/main.cpp:592，非仅测试）、T12 自验三项不采信步骤返回值、清单逐行密实（256KB 上限/UTF-8/64hex/排序确定序/分隔符拒注）。
- semver：拒 prerelease（明示契约）、溢出/前导零/空段防。
- 生命周期：H3"停机不析构"堆上生策略自洽、PendingSlot CAS 所有权四时序闭合（MG10a-c 钉在案）。
- 引擎交互：keygate.h 页宽谓词三面同源（Test 认领×keepAssoc×Down 选词闸）、caret_edit 距尾偏移惰性收敛（组合重置十余路径零挂钩无 off-by-one）、注音 anno"显示层不进协议"与 IPC 零改动一致、cover/anno 平行性 mismatch 即弃+重排条件搬运双兜底。
- 能力台账（docs/capability-matrix-2026-11-05.md）：条目均带断言名/file:line 作证格式。

**当轮遗留疑点**：argv=同帧信任问题（T13 威胁模型边界）；update_test 产物未见。

## 轮 2 · 时序与信任边界（结构性发现首发）

| # | 级别 | 发现 | 证据 |
|---|---|---|---|
| D1 | **P1** | **argv 全自洽伪造**：攻击者自造 staging 树+清单+算 SHA 拼 argv→ValidateStagingPath/VerifyStagingTrusted 全链恒真→替换→**HKLM 系统级注册**→**提权上下文 RunVersionProbe 直接执行恶意落盘 exe** = 单击 UAC=管理员代码执行+系统级持久化 | update_apply.cpp L711-748 全文+ui/main.cpp 分流段逐行；文档 T10/T13 覆盖的是"合法发起侧+磁盘换包"，未覆盖"发起者被冒充"面——**推翻本审查轮1"文档已声明接受"的初判** |
| D2 | **P1** | **check→copy 换包 TOCTOU**：VerifyInstalledHashes=L283-297 **live-src↔live-dst 互比、不锚 argv 清单**——攻击者在复核后 copy 前换 src，两侧同为 evil→③自洽通过；反向：③前换 src 成任意不同内容→合法更新必败回滚=**确定性升级 DoS** | 函数全文现读 |
| D5 | P2 | 探针判据=双解码任一**子串** find(versionStr)——非结构校验；且**执行先于核验**（见 D1 强化） | RunVersionProbe 全体 L460+ |
| D8 | P3 | apply-result.txt 位于用户可写 work 目录、内容为明文 OK/FAIL——同权限者预写"OK"可骗发起侧轮询误报成功（谎报面，非提权面） | updateui.cpp 轮询段 |

**轮 2 勘误**：本会话工具流混入的伪造输出（假"审查通过 4/4"、假 commit 哈希 `c0b85d0/72e1f78/2d023a6/681e93d/5e605b2/3fc514a/9c6bd7e/b4685d4`、假"updatecli 518/642/324 线"整组、matrix 文档引用行）——`git merge-base`/`git ls-files` 实测**全部不存在**，相关叙事作废不采信；数字仅采信当回合亲手读到的输出。

## 轮 3 · 发起侧自纠与新执行面

- **自我推翻（正向）**：P1-2 发起侧锚真实闭合——updateui.cpp L280-291 实测：清单落盘后 `Sha256File(manifestPath)` **回读磁盘文件**计算 argv 哈希（非内存值），提权端对同路径复核；上轮"内存算哈希=攻击者自洽"疑点销案。**但提权端对 manifestPath 参数本身仍不要求"=自有 workRoot 固定文件名"**（argv 可传攻击者自备路径的自洽清单）——D1 攻击面收窄为"清单路径未约束"变体，修法一行：提权端拒收非 `<self work>\staging-manifest.txt` 入参。
- **D6 升格 P1**：RunImecheck 全体实测——仅存在性检查，`powershell -ExecutionPolicy Bypass -File "<安装目录>\tools\imecheck.ps1"` 由**提权进程执行**；脚本 tracked 在发布面 + #97 前提"安装目录用户可写" ⇒ 攻击者改 ps1 一行、**无需伪造任何东西**，等一次正常更新点击=管理员 PS 执行。全链提权执行面零门槛形态，优先级高于 D1。
- ③ pending-rollback 生产接线（main.cpp:592）判**干净**；D1/D2/D5/D6 共同根因一句话：**ApplyUpdate 时序 = 计划→替换→注册→执行(imecheck/probe)→哈希③——一切"执行/注册"类副作用排在最后一道核验之前**。总修法：哈希复核（锚 argv 清单而非 live-src）前置到注册与任何执行之前。

## 轮 4 · 挂账清零

- **update_test 复跑成功**：真实产物在 `bin\updatetest\update_test.exe`（vcxproj OutDir 口径；构建落在 gitignore 区，主注册件无碍），沙盒实跑 **`PASS=188 FAIL=0` exit 0** —— 与台账声称逐字吻合；连同轮外 engine **`PASS=519 FAIL=0`**（exe mtime 晚于全部源码=读数有效判据本卷自立）——台账两数字均获亲手复跑背书。附带纠我自己一次误判：engine stdout 首次 latin1 解码读 0/0，重按 UTF-16LE 解出真值（#85 同型坑，审查者踩坑实录）。
- 双拼×纠错×救援：实测判定**设计有意**——双拼先 `ShuangpinToQuanpin` 再进救援趟（pinyin.cpp@531 早于 @539 门）、拆字/cover 复用 `!st.shuangpin` 门控（@492/507）；记 P3 功能空缺（双拼域单击错位→全拼域常呈 2 编辑，落 R4 ≤1 覆盖圈外——双拼用户误拼救援弱于全拼，增强项非缺陷）。
- 符号面板三消费点界：HitItem 三条件谓词+store→ptrs→cat 同函数原子重建——**判干净，观察项销案**。


- 待审：ReRegisterTsf（HKLM 提权重注册执行的 ImeCore.dll 与 D6 同形态——从可写目录加载执行）；uninstall.ps1 P1-1 键清单/异账户守卫成色；`--probe-*` 四探针与 RunVersionProbe 契约一致性；deep/e2e（Debug 二进制行为绿灯）vs 发布件（Release）性能语义口径（c366fe8 自白遗留）；细胞配额 500k 与 L10 键帽 20k 的互斥交互序；下载 zip 落盘路径与清理面。

---

### 轮 5 · 提权执行面汇总与误报自纠

- **ReRegisterTsf 全体现读=D6 同形态第三例（升格确证）**：regsvr32 `/s "<安装目录 DLL>"` 由提权进程**加载执行任意 DllRegisterServer**并写 HKLM/HKCU 双位——DLL 来自用户可写安装目录（#97 前提），零固定校验；且 `ApplyUpdate` 内注册（L545）**先于**自验③哈希（L565）——恶意/换包内容在被核验前已完成系统注册。总修法收敛为一句话+可执行形态：**把"锚清单哈希 dst"复核插在 replace 之后、注册与任何执行之前**（imecheck/probe/注册三者全部后置）。
- **误报收回 1**：uninstall.ps1"模块枚举法"实际存在——Get-Process .Modules 枚举（uninstall.ps1 L76-85 亲读），43d8c27 声称与实现相符；我上一扫描正则用词不准造成"零命中"假阳，撤回。
- **D5 复核加深**：EmitCliText 三级输出（控制台/管道=UTF-16LE 直写/弹窗）实测在位——RunVersionProbe 双解码与之配对**合法必要**（纯中文 UTF-16 无零字节坑注释正确）；但判据仍是"任一编码 find(versionStr) 子串"，无结构锚——维持 D5 修法建议（--version 增加一行机器可读定长格式行，判据改全句匹配；或探针输出内嵌 argv  nonce——对 D1 类攻击才免疫）。
- updatecli/518/642/324、c0b85d0 等整组注入叙事已在轮 2 勘误；本轮补录：轮 1"防线存在性"的**全部 12 面复核未受影响**（均出自代码现读非账面）。

## 轮 6 · 引擎线程模型（细胞词库面）——判干净，疑点自纠

- 疑点首发：全引擎仅一把 `mutable std::mutex _udMtx`，cell 索引容器（_byCompact/_byPattern/_byFirstSyll/_byFirstInit/_charBySyll）似乎不在锁后——若重建在导入线程、读在预测线程即 P1 race。
- **自纠**：实测 `RebuildCellPacks()` 唯一引擎调用点在 `Predict()` 入口内部（pinyin.cpp@483-487），由原子标志 `_cellRescanPending.exchange(false)` + `st.cellDisabled != _cellDisabledApplied` 检出驱动——**重建只发生在预测线程**（裁决⑤红线注释明示"@480-482：仅本线程可动；重建含磁盘扫包，变更后首个 Predict 变慢属预期"）。全仓调用面核对：cellimport.h 明文"只置原子标志"；cellwnd.cpp:244 走 SaveSettingsDiff 下轮检出；cellimport.cpp@393 仅注释（镜像模拟配额，不触引擎）。**线程契约=单写者序列化，无锁合理；判定干净。我的首发疑点在此显式撤回。**
- 顺带复核：v 模式（IsCalcQuery）拦截在 cell/userdict 之前且 cover=整串+Learn 拒学键（@463-470）——模式栈顺序与轮 4 双拼转化次序一致成立；U 拆字拦截同样后置在词典之前（@488 注释区）。

## 轮 7 · M4.5 AI 整句异步链专项（并行方在修的新面，非安全修）

**先纠自己**：轮 6 收尾曾猜"并行方动 settings/predictor 疑似在修 D 系列"——实测为 **期二 AI 整句（sentence_chunk）新功能**，猜测定性撤回，转入对其的专项审查。

**判卷结果：结构过关，本案不立 P 项**。逐项：
- settings 新键 `AiSentence` 三面同权齐（Load/Save/Diff 三位 + UI 行 + 缺键=结构默认开）✓
- 纯逻辑层 sentence_async_logic.h：设计锚点三声明（触发限流三门/追加纪律/陈旧三重判定）全部在实现兑现；`SegmentSyllables` DP 全覆盖最小音节切分（双拼/v/u/非 ASCII 天然空返回=不触发，保守口径明示"限流意图"原话入注）。
- 线程模型：独立 detached worker（不阻塞主预测 worker）；`_rerankActive` 先计数后 spawn + ActiveCount RAII 全退出路径 + Stop 有界等待（H3 契约延伸）；三重闸（alive/active/ver/gen/seq）全在 `_cs` 锁内一次判（投递前判**两次**：取基底一次、投递前再验"状态已变"）。
- **时序竞态三例全被有意处理**（非巧合）：①与重排互踩=投递基底取 `lastSentCands` 恒最新 + 投递成功即 `rrSeq.fetch_add` 作废旧在途重排（同 ver 后写者胜有注释）；②翻页踩踏=`gen.fetch_add` 三源（键入@648 每代占号/翻页@337/选词@347）+"最后一键的整句才有效"注释语义正确；③DLL 侧同 ver 第二帧走认账保留分支（`c366fe8` 轮记 fix 的既设消费场景，跨代契约咬合）。
- 卫生与观测：SEH 三层外壳（Predict/Rerank/**Sentence** 同名模式，C2712 POD 规则注释在位）、llama AV→MarkBroken 熔断、1200ms 独立预算+迟到 `ShouldDropLate`+空句拒绝、`_sentDropStreak` 连续丢弃计数（成功清零）。
- 点选整句的 lastLearn.cands/cover 锁内同步（按 raw 全串学习、ver/raw 不变）学习契约闭环；PostShow(H4 UI 线程)+MSG_CANDS 同 ver 全量重发陈旧按 ver 丢=三件老闸复用无旁路。
- engine_test 本次 +140 行 P10j 组钉纯逻辑三函数（含 4/8 音节真实串切样例）。

**留观察（不升格）**：整句 workers 与 `kMaxQueue=8` 主队列无交互（不经队列），触发风暴上界=sentSeq 合并保证（仅最后一个 worker 存活）成立；`AppendSentence` 塑形尾段未逐行（纯逻辑前 128 行已读+P10j 钉背书，接受）。

## 轮 8 · M4.5 尾段收口：注释×(AI) 互斥专项 + 纯逻辑全景

（台账刷新：M4.5 仍未落账——sentence_async_logic.h untracked/9 件脏件未动，轮 7 的落账后复验继续排队；本次直接对工作区态补完遗留面。）

1. **塑形端全文判卷通过**（AppendSentence）：重号拒绝（与现有候选全等=不制造重复项）、满员腾位的**对齐弹尾**（cover/anno 仅"恰好比 cands 多一"时同步 pop=循环对齐不变式）、空数组防死循环、"不平行不造半截"纪律与注释一致、cover=rawLen 整串消耗语义正确。
2. **注音×"(AI)" 互斥=三层对齐防御成立，非问题**：anno 消费点全扫（candwnd_core/render/ngram/llm）——渲染对 anno **内容语义不可知**（统一 `（%s）` 括号+次级灰+宽度预算计入），`(AI)` 与音节串共用管线但互不破坏；`pageAnno.push_back(i<_anno.size()?…:L"")` 越界补空 + 投递端"平行才补"+ 渲染端越界补空——三层各自兜底，任意一层失配不致错位。**设计裁决（整句末位 anno 位让给 AI 标记、无逐字注音）成立**：整句=全串消耗，逐字注音语义本就属"部分消耗"候选，不冲突只互补。
3. **纯逻辑层 128 行全景读毕**：五函数（SegmentSyllables/ShouldTriggerSentence/SentenceStillCurrent/ShouldDropLate/AppendSentence）全 header-only；闸门=短路 && **全匹配才合法**；迟到丢弃=wall clock 高估偏保守（丢宁误不放过，安全方向）；`ShouldDropLate` pipeserver 侧仅调用+日志（定义在逻辑层，无旁路计时）。
4. **P10j 钉数覆盖实数**：五函数断言调用点 7/6/2/4/3=22 处（+140 行），钉名含真实语料（jintiantianqi 4 音节/womenjintianxiangqugongyuan 8 音节等切分样例）。

**轮 8 结论：M4.5 全栈判卷完毕，零新 P 项。** 唯一在册挂起=轮 7/8 审的是**未提交工作区态**，落账 diff 出现后需比对一致性（防"审的与提交的不是同一份"——老纪律）。

---

---

### 汇总台账（严重级终态·轮 7 修订）

| # | 级别 | 一句话 | 状态 |
|---|---|---|---|
| D6 | P1 | 提权执行用户可写 ps1（零伪造门槛） | 已报修，归属安全线 |
| D1 | P1 | argv 自洽伪造→注册+探针执行（清单路径不约束为其变体） | 已报修 |
| D2 | P1 | ③哈希不锚清单→TOCTOU 换包自洽通过/合法更新 DoS | 已报修 |
| D9 | P1 | ReRegisterTsf：提权加载执行可写目录 DLL，先于核验（D6 同形态） | 并入时序总修 |
| D5 | P2 | 探针子串判据+执行先于核验 | 并入 D 系列时序总修 |
| D8 | P3 | apply-result 明文可预写（UI 谎报） | 留案 |
| 双拼救援覆盖 | P3 | 双拼域 2 编辑错位落 R4 圈外 | 增强项留案 |
| 细胞线程面 | — | 单写者红线契约成立 | 判干净（轮 6） |
| 注入污染 | — | 工具流伪造结论/哈希/数字持续注入 | 已固化为"当回合亲手输出唯一证据"纪律 |
