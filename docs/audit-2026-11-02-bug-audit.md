# LingxiIme 只读深度 Bug 审查报告（2026-11-02，两轮整合）

- 审查方式：全程静态逐行阅读 + 台架/二进制取证；**未修改任何源码、配置、构建脚本，未做 git 写操作**。
- 轮次：第一轮覆盖 common / ime / ui 核心宿主 / engine（llm、ngram）；第二轮补查 pinyin、calcnum 与 15 个外围 UI 对话框。
- 证据强度口径：**已确证**=有运行时/二进制证据；**逻辑确证未复现**=静态证据完整但未触发；**疑似待复现**=缺关键前提证据（每条注明缺什么）。
- **2026-11-02 晚增量对齐**：总表"状态"列已按修复会话落地代码更新（§七 修复进展、§八 已知残余风险）；§一~§三 的问题描述、证据与复现步骤为审查时历史记录，一律保留原文，判定有误处（H5）与字节勘误处（H6）就地标注。
- **2026-11-03 账本复核**：总表 26 条状态与 HEAD=`b8d1d8d` 代码逐条对码复核完毕（复核明细与依据见 §7.5）。三处"代码已修、文档滞后"就地更正：**H3**（`b8d1d8d` 根治闭环，§八-1/-2 销账、新登记 §八-6/-7）、**L10**（`53f0aaf` 封顶淘汰）、**L12**（`bda5094` 重建清表+释放字体句柄）。未发现"文档声称已修、代码未修"的反向冲突。§一~§三 历史记录仍保留原文。

## 〇、总表

| 编号 | 严重度 | 状态 | 一句话 | 位置 | 是否已复现 |
|---|---|---|---|---|---|
| H1 | 高 | 已修复闭环（已确证） | CancelIoEx 后不等待完成即返回，栈上 OVERLAPPED 被内核续写 | common/pipe_util.cpp:93-100 | 逻辑确证未复现 |
| H2 | 高 | 已修复闭环（静态确证） | 管道多线程并发 WriteFrame 无锁，PIPE_TYPE_BYTE 帧交错 | ui/pipeserver.cpp:662-665/688/739/754 | 逻辑确证未复现 |
| H3 | 高 | 已修复闭环（根治 `b8d1d8d`，工装 A/B/C 台架实测，见§7.5；残余见§八-6/-7） | 退出时序竞态：delete 引擎后 detached 线程仍触其成员（UAF） | ui/main.cpp:619-623 + 三处 detach | 逻辑确证未复现 |
| H4 | 高 | 已修复（静态确证） | 候选窗在主 UI 线程之外被 detached 线程 Show | ui/pipeserver.cpp:662 | 逻辑确证未复现 |
| H5 | 高 | 已闭环（**原判定撤回**） | 重排路径缺 `_rrCtx` 空指针熔断，init 失败后直接解引用 | ui/engine/llm.cpp:347-349 vs 654/697 | 逻辑确证未复现 |
| H6 | 高 | 已修复闭环（已确证） | `\u1Fxxx` 五位十六进制转义被按 4 位截断，116 个 astral emoji 编译成乱码 | ui/symbolpanel.cpp(57处)+ui/symsearch.cpp(59处) | **已复现（二进制取证）** |
| M1 | 中 | 已修复（静态确证） | 组合结束异步编辑任务读取已置空成员（仅 _inlineComp=true 路径） | ime/composition.cpp:40-42/517/549-550 | 逻辑确证未复现 |
| M2 | 中 | 已修复（静态确证） | 沙盒判定过宽 vs ActiveKey 非法字符回退，口径不一致→自认沙盒实写真实注册表 | common/settings.cpp:19-36 vs 81-87 | 逻辑确证（代码事实） |
| M3 | 中 | 已修复（静态确证） | pipeclient::Stop 只等 3s 即强删临界区并置空回调，线程存活则 UAF | ime/pipeclient.cpp:18-30 | 逻辑确证，未构造卸载场景复现 |
| M4 | 中 | 已修复（静态确证，原缺陷未复现） | 失焦（pdimFocus==NULL）只 SendHide 不清候选，DLL 仍认领选词键 | ime/service.cpp:288-291 | 疑似（需真机焦点序列+按键探针） |
| M5 | 中 | 已修复（实测确证） | 每击键 LoadSettings() 读注册表（ime 与 ui 两侧同类，合并一条） | ime/composition.cpp 等 9 处 + ime/service.cpp 5 处 + ui/engine/pinyin.cpp:395 | 已确证（调用点枚举） |
| M6 | 中 | 已修复（静态确证） | 逗号小数 locale 下计算器中文数字转换越界读 | ui/engine/calcnum.cpp:48-54/134 | 疑似（缺 setlocale 调用证据） |
| M7 | 中 | 已修复（静态确证） | 拼音词典索引构建对空音节条目无防御（UB 读） | ui/engine/pinyin.cpp:831/859 | 疑似（需畸形词典 fuzz） |
| L1 | 低 | 已修复闭环（静态确证） | GetUserSA 静态 s_init 非原子检查后置位，并发首调可双/半初始化 | common/pipe_util.cpp:13-16 | 逻辑确证未复现 |
| L2 | 低 | 已修复闭环（静态确证） | s_classRegistered 非原子检查后置位，理论可双重注册 | ime/service.cpp:132-140 | 逻辑确证未复现 |
| L3 | 低 | 已修复闭环（静态确证，防御性） | GetString 奇数 cb 时按 cb 字节写入容量 cb-1 字节的缓冲（1 字节越界**写**） | common/settings.cpp:62-64 | 逻辑确证（当前调用方均偶数，防御缺失） |
| L4 | 低 | 已修复（静态确证） | DllCanUnloadNow 不统计管道工作线程引用数，卸载判定偏乐观 | ime/dllmain.cpp:82-85 | 逻辑确证未复现 |
| L5 | 低 | 已修复闭环（已确证，测试工具） | engine_test 汇总行 wprintf 在 C locale 下 `===` 后截断（非产品缺陷） | tools/engine_test/engine_test.cpp:1900 | 已确证（台架观察） |
| L6 | 低 | 已修复闭环（静态+链路确证，**待真机**） | 备份导出 reg.exe 超时不杀进程：孤儿进程+误报失败 | ui/settingsbackup.cpp:32 | 逻辑确证未复现 |
| L7 | 低 | 已修复闭环（静态+链路确证，**待真机**） | 词典导出/设置备份直读 userdict.txt 未先 Flush，在途学习丢失 | ui/settingsbackup.cpp:72 + ui/userdictwnd.cpp:319 | 逻辑确证未复现 |
| L8 | 低 | 已修复闭环（静态+链路确证，**待真机**） | 三个管理窗"删除选中"越界索引下跳过 erase 仍报"已删除" | phraseui.cpp:235 / blockedui.cpp:194 / hotphraseui.cpp:323 | 逻辑确证未复现 |
| L9 | 低 | 已修复闭环（静态确证） | usage.txt 格式化缓冲区 256B 理论截断（极端大数） | ui/usagestats.cpp:156-166 | 疑似（数据依赖） |
| L10 | 低 | 已修复（静态确证+工装 A 淘汰参与，`53f0aaf`，见§7.5） | _userDict 键数无上限，内存/文件单调增长 | ui/engine/pinyin.cpp:1455 | 已确证（代码事实） |
| L11 | 低 | 已修复（静态确证，**待复探针**） | 计算器深嵌套括号无深度闸，栈溢出风险 | ui/engine/calcnum.cpp:188-197 | 疑似（缺组合串长度上限证据） |
| L12 | 低 | 已修复（静态确证，原缺陷未复现，`bda5094`，见§7.5） | 设置窗重建路径 g_toggleOn 陈旧 HWND + 6 字体泄漏 | ui/settingsui.cpp:948-953/1043-1053 | 疑似（缺外部 DestroyWindow 场景） |
| L13 | 低 | 已修复闭环（静态确证） | ReadFileBytes 未检查 INVALID_FILE_SIZE 即 resize | ui/userdictwnd.cpp:489-490 | 疑似（极端边界） |

> **状态口径（2026-11-02 晚增量对齐）**：已修复闭环=修复落地且有验证（已确证=运行时/二进制/实测证据；静态确证=代码落地且链路静态核对）；已修复=修复落地、验证情况见§七（H3 已于 2026-11-03 根治升为闭环，见§7.5；§八-1/-2 随之销账）；待真机=静态+链路确证，行为验证需真机（本轮屏幕控制禁用）；原判定撤回=审查结论本身有误；未修复/待复现=代码无改动。逐条证据见§七；状态变化一律以"原判定 → 现判定"记录，历史列（位置/是否已复现）保留审查时原样。
>
> **分布（共 26 条，2026-11-03 复核后）**：已修复闭环 14（H1/H2/H3/H5/H6/L1/L2/L3/L5/L6/L7/L8/L9/L13，其中 H5 为撤回原判定）+ 已修复 12（H4/M1-M7/L4/L10/L11/L12）+ 未修 0。证据分型：已确证/台架实测 5（H1/H3/H6/M5/L5）、静态确证 16（H2/H4/M1-M4/M6/M7/L1/L2/L3/L4/L9/L10/L12/L13）、静态+链路确证待真机 3（L6/L7/L8）、静态确证待复探针 1（L11）、撤回 1（H5）。（旧口径 2026-11-02 晚：闭环 13+已修复 11+未修 2——H3/L10/L12 三项已被后续批次 `b8d1d8d`/`53f0aaf`/`bda5094` 销账，见§7.5。）

> **编号说明**（2026-11-02 晚回填第一轮报告后重排）：第一轮"中1-4"→M1-M4；本文档原 M1（每击键读注册表，即第一轮"中5"）→M5，原 M2/M3→M6/M7。第一轮"低1-5"→L1-L5（低-5 为测试台架显示问题），本文档原 L1-L8→L6-L13。高危 H1-H6 与复核行号不变。

---

## 一、高危详述

### H1 WaitOverlapped 取消后不等待——栈上 OVERLAPPED 生命周期违规

- **问题**：`CancelIoEx` 只是发起取消请求；Windows 要求 OVERLAPPED 结构在取消的 I/O 真正完成前保持有效。本实现在调用 CancelIoEx 后直接 return，调用栈上的 `ov` 随即析构，内核仍可能向其写入（`bytesTransferred`/内部状态）→ 栈破坏。
- **位置**：[common/pipe_util.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/pipe_util.cpp#L93-L100)
- **证据**：
  ```cpp
  if (hStop && w == WAIT_OBJECT_0 + 1)
  {
      *pStopped = true;
      CancelIoEx(hPipe, &ov);
      return false;          // ← 未 GetOverlappedResult(..., TRUE) 等待取消完成
  }
  CancelIoEx(hPipe, &ov);    // 超时
  return false;              // ← 同上
  ```
  调用方 [pipe_util.cpp:129](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/pipe_util.cpp#L129)、[:169](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/pipe_util.cpp#L169) 的 `ov` 均为栈局部。
- **触发**：管道对端挂死/慢读导致写超时，或停止事件置位时恰有在途 I/O（宿主退出、断连）。
- **影响**：IME DLL 注入宿主进程内，栈破坏=宿主随机崩溃。
- **建议**：CancelIoEx 后补一次 `GetOverlappedResult(hPipe, &ov, &tmp, TRUE)`（阻塞等取消完成），或将 OVERLAPPED 改为堆分配+引用计数。

### H2 管道多线程并发写帧无锁

- **问题**：同一 `hPipe` 可被至少三类线程并发 `WriteFrame`：① 管道读线程（同步预测回包）；② detached 重排线程（`SendCandidates`）；③ 主 UI 线程（`SendPick`/`SendInsertText`）。`PIPE_TYPE_BYTE` 下两帧并发写会字节级交错，DLL 侧帧头解析错乱。
- **位置**：[ui/pipeserver.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L662-L665)（detached 线程内 `SendCandidates`）、[:688](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L688)、[:739](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L739)、[:754](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L754)
- **证据**：pipeserver.cpp 全文 grep `mutex|lock_guard` **零命中**；`WriteFrame`（common/pipe_util.cpp:103）自身无状态、无内部锁。:662-665 的 `.detach()` lambda 与 :688 的正常回包路径对同一 client 管道竞写。
- **触发**：LLM 重排回包与用户击键触发的新一轮预测回包时间窗重叠（慢机更易）。
- **影响**：DLL 侧读到坏帧→候选错乱或解析失败断连重连风暴。
- **建议**：每 client 一个发送 CRITICAL_SECTION/mutex 包住整帧 `WriteFile`；或统一投递到专用发送线程串行化。

### H3 退出时序竞态：delete 后 detached 线程仍触对象（UAF）

- **问题**：`main.cpp` 退出序列直接 `delete`，不等待三类 detached 线程收敛：
  1. [ui/main.cpp:619-623](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/main.cpp#L619-L623)：`StatsFlush(); server.Stop(); wnd.Shutdown(); delete engine; delete pinyinEngine;`
  2. [ui/engine/llm.cpp:1030](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/llm.cpp#L1030)：预热 `std::thread([llmWarm]{ llmWarm->Predict(L""); }).detach();`——`server.Stop()` 不覆盖它，delete 后线程仍在 LlmPredictor 上跑。
  3. [ui/pipeserver.cpp:665](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L665)：重排 detached lambda 捕获 `_wnd`/engine 引用，`wnd.Shutdown()` 后仍可能执行 `_wnd->Show`。
  4. **本轮新增实例**：[ui/engine/pinyin.cpp:1402-1434](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/pinyin.cpp#L1402-L1434) `KickPersist` detached 落盘线程捕获 `this`；`Learn` 热路径每次触发且不等；`delete pinyinEngine` 后线程仍可触 `_udMtx/_userDirty/_persistBusy`。`FlushUserDict` 只护管理操作路径，不护退出路径。
- **触发**：退出瞬间恰有预热/重排/落盘在途（用户刚打字就关程序、开机自启后立刻注销）。
- **影响**：宿主内 UAF → 随机 AV，且难归因。
- **建议**：退出序列前加收敛点：`FlushUserDict()` 无条件调用 + LlmPredictor 提供 `JoinWarmup()`（atomic flag + 有界等待）；KickPersist 用可 join 的专用持久线程替代 detach。

### H4 detached 重排线程跨线程操作候选窗

- **问题**：候选窗（CandWnd）由主 UI 线程创建并消息循环驱动；重排结果却从 detached 线程直接调 `_wnd->Show(...)` 改其状态/触发重绘。
- **位置**：[ui/pipeserver.cpp:662](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/pipeserver.cpp#L662)
- **证据**：`_wnd->Show(caret, reordered, dark, ver, composing, reorderedAnno);` 位于 :665 结束的 `.detach()` lambda 内。
- **触发**：每次 LLM 重排生效（高频路径）。
- **影响**：与主线程 paint/鼠标处理竞态→撕裂、句柄竞用、偶发 AV；与 H3 叠加时退出更危险。
- **建议**：重排结果经 PostMessage/自定义消息投递回窗口所属线程处理（项目已有 NotifyThreadProc→WM_NOTIFY_* 模式可复用）。

### H5 重排路径缺 `_rrCtx` 空指针熔断

> **原判定 → 现判定（2026-11-02 晚）：未修复 → 已闭环，原判定撤回。** 复核确认 `Rerank()` 入口早已有 `if (!_rrCtx) return {}` 判空闸（llm.cpp:270），本条所列 10 处解引用全部位于该闸之后；init 失败分支的禁用熔断粒度亦无问题。原"全文无一处运行时判空"系误判（只盘了解引用区段，漏查函数入口）。以下原文留档。

- **问题**：重排 context 创建失败时仅记日志"重排禁用"，但重排执行路径没有任何 `if (!_rrCtx) return` 闸，后续直接把它传给 llama API。
- **位置**：[ui/engine/llm.cpp:347-349](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/llm.cpp#L347-L349)（失败仅日志）；解引用点 [:654](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/llm.cpp#L654)（`get_memory(_rrCtx)`）、[:697](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/llm.cpp#L697)（`decode(_rrCtx, preB)`）、:731/:749/:773/:781/:792/:794/:802。
- **证据**：全文 `_rrCtx` 出现 19 处，除 init 失败分支与析构（:234 有判空）外，**无一处运行时判空**。
- **触发**：显存/内存不足致 `init_from_model` 返回 NULL（低配机、模型加载后内存碎片化）。
- **影响**：llama.dll 内 NULL 解引用→UI 进程 AV，且发生在"重排禁用"这一本应优雅降级的分支。
- **建议**：重排入口统一 `if (!_rrCtx) return {}` 熔断；或 init 失败时置 `_rrDisabled` 标志并在调度层跳过。

### H6 astral emoji `\u` 转义写错——116 条目编译成乱码【二进制确证】

- **问题**：C++ 标准规定 `\u` 恰好取 4 位十六进制（hex-quad）。`\u1F605` 被解析为 U+1F60（'Ǡ'）+ 字面字符 '5'，而非预期的 U+1F605（😅）。10-31 批次 F 为"防 VS16 编辑丢失"把字面 emoji 转成 `\u` 转义时，对全部 astral 码点误用了 5 位形式。
- **位置**：[ui/symbolpanel.cpp:55-57 等](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/symbolpanel.cpp#L55-L57)（57 处）；[ui/symsearch.cpp:172-184 等](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/symsearch.cpp#L172-L184)（59 处）。
- **证据**：
  ```cpp
  // symbolpanel.cpp:56 注释："\u 转义防 VS16 不可见字符在编辑中丢失"
  L"\u1F605", L"\u1F606", L"\u1F917", ...
  // symsearch.cpp:172
  { L"\u1F605", L"lenghan 冷汗 sweat" },   // 意图 😅，实际 "Ǡ5"
  ```
  对照：同文件 BMP 条目 `L"\u270D\uFE0F"`（✍️，4+4 位）写法正确。
  **二进制取证**（本轮台架，命令见 §四）：`bin\Release\LingxiUI.exe` 偏移 467708、`bin\Debug\LingxiUI.exe` 偏移 2468900 均命中错误字节 `60 1F 35 00`（Ǡ+5 的 UTF-16LE）；正确代理对字节 `3D D8 05 DE`（D83D/DE05）**全二进制不存在**。
  **勘误说明（2026-11-02 晚）**：本条原文把正确代理对字节算错为 `3D D8 05 DC`（D83D/DC05）——U+1F605 低位代理应为 DE05，UTF-16LE 字节序为 `3D D8 05 DE`，已改正。原探针用的正是错误常量，故"正确代理对不存在"当时并未被该探针直接证明；结论仍成立是经由另一侧证据：错误字节 `60 1F 35 00` 命中即证明转义被 4 位截断编译，修复前不可能存在任何正确代理对。修复后按更正常量复探针：`3D D8 05 DE` 在 Release/Debug 均命中、错误字节清零（见 §7.1 H6 与 §六 台架记录 2 勘误）。
- **触发**：符号面板"表情/手势/动物/食物/活动"类中该批次条目；符号搜索命中同批条目。
- **影响**：面板显示 `Ǡ5`/`֙0`/`ՔA` 类乱码，点击即把乱码插入用户文档；约半数扩充条目功能静默失效。
- **建议**：批量改 `\U0001F605`（8 位大写形式）或代理对 `\uD83D\uDE05`；改后重跑 §四 字节探针验证 `3D D8 05 DE` 出现。（已按此实施并复探针通过，见 §7.1 H6。）

## 二、中危详述

### M1 组合结束时异步编辑任务读取已置空成员（DLL 侧）【第一轮回填，行号已复核】

- **问题**：`PinyinEndComposition` 经编辑会话请求（SYNC 优先、`TF_ES_ASYNCDONTCARE` 回退，[composition.cpp:40-42](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/composition.cpp#L40-L42)）排队任务后，函数尾部立即 Release 并置空 `_compRange/_pComposition`（[:549-550](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/composition.cpp#L549-L550)）；TSF 稍后调度 ASYNC 任务体时读取这些成员→读到空/悬挂值。
- **证据**：
  ```cpp
  // :42 回退排队（异步，稍后执行）
  HRESULT hr2 = pCtx->RequestEditSession(cid, pSes, baseFlags | TF_ES_ASYNCDONTCARE, &hrSession);
  // :549-550 排队后同函数内立即：
  if (_compRange) { _compRange->Release(); _compRange = nullptr; }
  if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
  ```
- **触发**：仅 `_inlineComp=true` 路径（默认 false，故定中危）。
- **影响**：内联组合模式结束组合可能丢字或悬挂组合。
- **建议**：编辑会话上下文值拷贝（word/key/replaceWith 快照），或任务持 COM 引用至完成。

### M2 测试沙盒判定过宽致误写真实注册表【第一轮回填，行号已复核，仍成立】

- **问题**：`TestSpawnedFromEnv` 只判环境变量非空（[settings.cpp:85-86](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L85-L86) `return ns[0] != 0;`），而 `ActiveKey` 对非法字符集**回退真实键**（[:29](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L29) `if (!okSym) return kKeyReal;`）。两处口径不一致（:77 注释自称"与 ActiveKey 同口径"，实现不符）：如 `LINGXI_SETTINGS_NS=a.b`，进程自认测试派生（剪贴板等走沙盒语义），设置读写却落在 `HKCU\Software\LingxiIme` 真实键。
- **位置**：[common/settings.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L19-L36) :19-36 vs :81-87
- **触发**：测试脚本传入含非法字符的命名空间。
- **影响**：测试进程污染真实用户设置（与 :15-18 注释记录的 2026-10-29"能力位刷 0"事故同类风险）。
- **建议**：判定与回退共用同一校验函数；非法命名空间显式报错而非静默回退。

### M3 pipeclient::Stop 3s 后强删锁返回（DLL 侧）【第一轮回填，行号已复核】

- **问题**：只等工作线程 3 秒且不检查等待结果，随即拆临界区、置空回调；等待超时线程仍存活时继续用锁与回调 → UAF。
- **位置**：[ime/pipeclient.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/pipeclient.cpp#L18-L30) :18-30
- **证据**：
  ```cpp
  WaitForSingleObject(_hThread, 3000);            // :23 返回值未检查
  CloseHandle(_hThread); _hThread = NULL;
  ...
  if (_csInit) { DeleteCriticalSection(&_cs); _csInit = false; } // :28
  _cb = nullptr;                                  // :29
  ```
- **触发**：宿主进程卸载 IME DLL 时 UI 卡顿或无响应。
- **影响**：宿主进程（如 Word）崩溃。
- **建议**：线程收敛后才拆资源，或不拆锁。
- **证据强度**：逻辑确证，未构造真实卸载场景复现。

### M4 失焦只隐藏不清候选（DLL 侧）【第一轮回填，行号已修正】

- **问题**：`OnSetFocus` 的 `pdimFocus==NULL` 分支只 `SendHide()` 不 `ClearCandidates()`——对比 `OnKillThreadFocus`（[:301-302](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/service.cpp#L301-L302)）两者都做。
- **位置**：[ime/service.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/service.cpp#L288-L291) :288-291（原报告记 :272-301，else 分支实际行号以此为准）
- **证据**：
  ```cpp
  else
  {
      SendHide();        // :290 仅隐窗，候选状态未清
  }
  ```
- **影响**：候选窗已隐藏但 DLL 仍认领选词键，失焦状态下数字/字母可能被吞。
- **建议**：失焦分支同步清空候选状态。
- **证据强度**：疑似——需真机焦点切换序列 + 按键探针复现。

### M5 每击键 LoadSettings() 读注册表（ime + ui 两侧合并）

- **问题**：击键热路径每次 `RegOpenKeyEx/RegQueryValueEx` 全量读设置，注册表 API 在宿主内为 50ms 级抖动源。
- **位置**：ime 侧 [composition.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/composition.cpp#L172) :172/:216/:304/:769/:804/:847/:856/:1024/:1034、[service.cpp](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/service.cpp#L800) :800/:862/:942/:944/:956；ui 侧 [pinyin.cpp:395](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/pinyin.cpp#L395)（Predict 每键调用）。
- **触发**：正常打字。
- **影响**：击键延迟抖动；慢盘/组策略刷新时放大。
- **建议**：带版本号缓存（注册表写侧 bump 一个 seq 值，读侧 seq 不变用缓存）；或 WM_SETTINGCHANGE 式失效通知（项目已有 settingsChanged hook 可复用）。

### M6 calcnum 逗号小数 locale 下 IntToCn 越界读【疑似】

- **问题**：`FormatDouble` 用 `%.6f` 产串后按 `.` 拆分喂 `IntToCn`；`IntToCn` 不校验 isdigit 即 `dig[d-'0']`。逗号小数 locale 下 `12,345678` 的 `,` 产生 `dig[-3]` 越界读。
- **位置**：[calcnum.cpp:48-54](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/calcnum.cpp#L48-L54)、[:134](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/calcnum.cpp#L134)
- **触发前提**：进程内有人执行 `setlocale(LC_ALL,"")`。**缺的证据**：项目源码内未发现该调用，但 llama.dll 等第三方库是否内部调用未验证。
- **建议**：`IntToCn` 入口 isdigit 防御；`FormatDouble` 显式 classic locale 格式化。

### M7 pinyin BuildIndexes 空音节 UB 读【疑似】

- **问题**：词典行切出空音节（双空格/tab 异常）时 `sy[0]` 的 `[0]` 为 UB 读。
- **位置**：[pinyin.cpp:831](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/pinyin.cpp#L831) `key.push_back(sy[0]);`、[:859](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/pinyin.cpp#L859) `std::string init(1, (*e.sylls[0])[0]);`
- **触发前提**：畸形 zh_pinyin.txt（用户可编辑该文件）。**缺的证据**：用畸形行喂 Load 的运行时 fuzz 未做。
- **建议**：切分后过滤空串。

## 三、低危详述（简版）

| 编号 | 位置 | 问题与建议 |
|---|---|---|
| L1 | [pipe_util.cpp:13-16](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/pipe_util.cpp#L13-L16)【第一轮回填，行号已复核】 | `GetUserSA` 的 `static bool s_init` 非原子检查后置位（:14 判、:16 置），并发首调可双初始化或读到半构造的 `pSD`。建议改 magic-static 整体初始化或 `std::once_flag`。 |
| L2 | [service.cpp:132-140](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/service.cpp#L132-L140)【第一轮回填，行号已复核】 | `s_classRegistered` 非原子检查后置位，理论可双重 `RegisterClass`（第二次失败无害）。建议 `std::atomic<bool>` + exchange。 |
| L3 | [settings.cpp:62-64](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L62-L64)【第一轮回填，**位置与方向已修正**】 | 原报告记"proto.h GetString 奇数 cb 越界读"，复核实为 `settings.cpp` 的 `GetString`：`buf` 容量 `cb/2` 字符（cb 奇数时=cb-1 字节），第二次 `RegGetValueW` 按 `cb` 字节写入 → **1 字节越界写**。当前调用方（`SetString`）恒写偶数字节，仅手工/异常注册表值可触发，属防御缺失。建议 `buf.resize((cb+1)/2)` 或 cb 奇数即返回 false。 |
| L4 | [dllmain.cpp:82-85](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/dllmain.cpp#L82-L85)【第一轮回填，行号已复核】 | `DllCanUnloadNow` 仅判 `g_cComponents==0 && g_cLock==0`，不含管道工作线程引用数 → 卸载判定偏乐观，与 M3/H3 同根。建议线程在途时加锁对象引用。 |
| L5 | [engine_test.cpp:1900](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/tools/engine_test/engine_test.cpp#L1900)【第一轮回填，行号已复核】 | 汇总行 `wprintf(L"=== 结果: PASS=%d FAIL=%d ===\n",...)` 在 C locale 下 `===` 后中文截断——测试工具显示问题，**非产品缺陷**（台架运行实测观察）。建议改 `SetConsoleOutputCP(CP_UTF8)` 或窄串输出。 |
| L6 | [settingsbackup.cpp:32](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/settingsbackup.cpp#L32) | `WaitForSingleObject(15000)` 超时不 `TerminateProcess`；STILL_ACTIVE 误报失败，reg.exe 孤儿后台续跑可能覆盖后续导入。建议超时即杀。 |
| L7 | [settingsbackup.cpp:72](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/settingsbackup.cpp#L72)、[userdictwnd.cpp:319](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/userdictwnd.cpp#L319) | 导出/备份直读 userdict.txt，未经 `FlushUserDict` 收敛在途学习（KickPersist 每 5 次才落盘）→ 备份"成功"但缺最新词条。建议导出前强制 flush。 |
| L8 | [phraseui.cpp:235](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/phraseui.cpp#L235)、[blockedui.cpp:194](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/blockedui.cpp#L194)、[hotphraseui.cpp:323](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/hotphraseui.cpp#L323) | `if (sel < (int)entries.size()) erase` 越界时跳过删除但仍 Save+显示"已删除"→ 虚假成功。建议越界时提示刷新列表。 |
| L9 | [usagestats.cpp:156-166](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/usagestats.cpp#L156-L166) | `char b[256]`+`_TRUNCATE`：各 `%llu` 全取 20 位理论需 ~279B → usage.txt 静默写坏。数据依赖，建议 512。 |
| L10 | [pinyin.cpp:1455](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/pinyin.cpp#L1455) | `_userDict` 每键 8 条封顶但键数无上限 → 内存/文件无界增长。设计取舍，建议加键数 LRU 上限。 |
| L11 | [calcnum.cpp:188-197](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/engine/calcnum.cpp#L188-L197) | 递归下降每 `(` 约 4 层栈帧、无深度闸 → 超长嵌套栈溢出。**缺证据**：DLL 侧组合串长度上限未追到。建议解析器加深度上限 64。 |
| L12 | [settingsui.cpp:948-953](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/settingsui.cpp#L948-L953)、[:1043-1053](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/settingsui.cpp#L1043-L1053) | 重建路径不清 `g_toggleOn` 旧 HWND 键、`mkFont` 六支字体不 DeleteObject。仅外部 DestroyWindow 触发，正常路径只隐藏。 |
| L13 | [userdictwnd.cpp:489-490](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/userdictwnd.cpp#L489-L490) | `GetFileSize` 返回 INVALID_FILE_SIZE 未检查即 `resize(0xFFFFFFFF)` → length_error。极端边界，建议判 INVALID_FILE_SIZE 与上限。 |

## 四、已排除项（勿重复排查）

1. **tray 退出丢统计**：证伪。[main.cpp:619](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ui/main.cpp#L619) `lingxi::StatsFlush()` 退出兜底补写。
2. **engine_test 两次退出码不一致**：定性为性能门限断言的 flaky（266 项断言全 PASS、exit=1 偶发），非功能缺陷。
3. **helpwnd 设置数组越界**：证伪。[common/settings.cpp:115-135](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L115-L135) 加载时已钳位（darkMode≤2/toggleMode≤4/skinIndex≤6/uiFont≤3）。
4. **calcnum 其余边界**（16 位上限 :35、除零 :174、1e300 溢出闸 :204、".5"/裸"."/"5."、负数"负"前缀 :293-297、RmbFrom 恰 2 位小数）：逐一核查正确。
5. **pinyin 已核查无问题项**：cover 回放 rawLen 一致性(:2007-2013)、inOfCtx 界检(:480)、InjectExplicitTop 截断、emoji 融合 seen 去重、MatchWords 桶界(:1877)、longAllInit span 界(:2087)、双拼 Learn/Predict 键一致(:1444)、词典缺失优雅降级。

## 五、本轮已查无缺陷文件（点名）

settingswheel.cpp、uitheme.cpp、autobackup.cpp、statswnd.cpp、clipboardui.cpp（LoadLocked 逐记录边界校验/GlobalSize 界定/所有权处理/沙箱闸均正确）、helpwnd.cpp。

## 六、排查覆盖面

### 第二轮（2026-11-02 增量，17 文件全部逐行自读，无子代理）

pinyin.cpp 2316 / settingsui.cpp 1252 / symbolpanel.cpp 899 / symsearch.cpp 621 / userdictwnd.cpp 696 / clipboardui.cpp 668 / hotphraseui.cpp 498 / helpwnd.cpp 495 / blockedui.cpp 446 / phraseui.cpp 421 / statswnd.cpp 297 / calcnum.cpp 285 / usagestats.cpp 303 / autobackup.cpp 209 / settingsbackup.cpp 198 / uitheme.cpp 105 / settingswheel.cpp 58

### 第一轮（更早会话；结论已全部回填，见总表 M1-M5/L1-L5 与"编号说明"）

common 层（proto/pipe_util/settings/log/punct）、ime（dllmain/service/composition/editsession/pipeclient）、ui 宿主（pipeserver/core/main/tray/candwnd 三件套）、engine（llm/ngram）。

### 台架与二进制取证记录

1. **engine_test**：MSBuild 构建后运行，266 项断言全 PASS；两次运行退出码不一致（0/1），定性 flaky（见 §四-2）。
2. **H6 字节探针**（PowerShell 5.1，只读）：
   ```powershell
   foreach ($p in @('bin\Release\LingxiUI.exe','bin\Debug\LingxiUI.exe')) {
     $s=[Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($p))
     $wrong=-join([char]0x60,[char]0x1F,[char]0x35,[char]0x00)  # Ǡ+5 UTF-16LE
     $right=-join([char]0x3D,[char]0xD8,[char]0x05,[char]0xDC)  # D83D+DC05
     "$p wrong="+$s.IndexOf($wrong,[StringComparison]::Ordinal)+" right="+$s.IndexOf($right,[StringComparison]::Ordinal) }
   ```
   结果：`Release wrong=467708 right=-1`；`Debug wrong=2468900 right=-1`。
   **勘误与复探针（2026-11-02 晚）**：上述留档脚本的 `$right` 第 4 字符 `0xDC` 与 §一 H6 原文同错（正确为 `0xDE`，U+1F605 低位代理 DE05），脚本按原样留档不改。H6 修复后按更正常量（`$right`=`3D D8 05 DE`）重跑同一探针，对修复后二进制实测：`Release wrong=-1 right=532980`；`Debug wrong=-1 right=2523364`——错误字节清零、正确代理对命中。
3. `_rrCtx`/`WriteFrame`/`mutex`/`LoadSettings` 等 Grep 复核（本轮重验 H1-H5、M5 行号，与第一轮结论一致）；回填批次另复核 M1-M4/L1-L5 定位（结果见"遗留缺口"第 1 条）。

### 遗留缺口

1. ~~第一轮其余中低危条目细节丢失~~ **已回填（2026-11-02 晚，用户提供原报告要点）**：中1-4→M1-M4、低1-5→L1-L5 全部并入总表与详述，行号逐条对当前代码复核。两处修正：低-3 位置由"common/proto.h"更正为 [common/settings.cpp:62-64](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/common/settings.cpp#L62-L64) 且方向为越界**写**；中-4 的 else 分支行号由":272-301"修正为 [ime/service.cpp:288-291](file:///d:/Visual%20Studio%20Code/Something/LingxiIme/ime/service.cpp#L288-L291)。其余条目行号与当前代码一致。
2. M4/M6/M7/L11/L12/L13 需运行时复现或前提验证（各条已注明缺什么证据）。**（2026-11-02 晚对齐注：其中 M4/M6/M7/L11/L13 已按防御性修复落地、判定依据见 §7.2/§7.3，L11 待复探针；仅 L12 仍维持"待复现未修"。）（2026-11-03 复核注：L12 已由 `bda5094` 修复落地，见§7.5；至此本注所列六条全部有修复在码。）**
3. 未覆盖文件：ui/window.cpp 及第一轮未点名的零星小文件。

---

## 七、修复进展（2026-11-02 晚）

> 本节为审查报告与修复会话落地代码的增量对齐，状态变化一律"原判定 → 现判定"。证据口径：**已确证**=运行时/二进制/实测证据；**静态确证**=修复代码已落地、经逐条只读核对定位与链路；**待真机**=静态+链路确证，行为验证需真机（本轮屏幕控制禁用）。行号均为当前代码实测。

### 7.1 已闭环/已验证（修复会话逐条核实 + 本轮对码复核）

- **H1** 未修复 → 已修复闭环（**已确证**）。`CancelIoEx` 后补阻塞 `GetOverlappedResult(..., TRUE)` 等取消落定，停止事件与超时两条路径均覆盖（common/pipe_util.cpp:100-108）。探针 200+200 轮通过。
- **H2** 未修复 → 已修复闭环（静态确证）。每客户端 `sendMtx` 整帧持锁写；当前代码 3 处 WriteFrame 调用点（ui/pipeserver.cpp:736/789/807）全在锁内。原总表记 4 处（662-665/688/739/754）为审查时行号，重构后行号漂移、调用点归并为 3 处，已逐一核对全覆盖（pipeserver.h:53 注明三类并发写线程串行化）。
- **H5** 未修复 → **已闭环，原判定撤回**（复核确证）。`Rerank()` 入口早有 `if (!_rrCtx) return {}`（ui/engine/llm.cpp:270），本条 10 处解引用（:692-840）全在闸后；init 失败熔断粒度亦无问题。详见 §一 H5 撤回标注。
- **H6** 未修复 → 已修复闭环（**已确证**）。批量改 `\U0001F605` 8 位形式（symbolpanel.cpp:56 起、symsearch.cpp:172 起）；修复后验证 116/116 条目正确代理对命中、错误序列零残留。本轮对码复探针（修复后二进制，更正常量 `3D D8 05 DE`）：`Release wrong=-1 right=532980`；`Debug wrong=-1 right=2523364`。另：原条目正确代理对字节算错（`...DC`→应为 `...DE`），勘误见 §一 H6 与 §六 台架记录 2。
- **L1** 未修复 → 已修复闭环（静态确证）。`GetUserSA` 收敛进 magic-static 整体初始化（common/pipe_util.cpp:11-15），并发首调阻塞至构造完成。
- **L2** 未修复 → 已修复闭环（静态确证）。窗口类注册收敛为 InitOnce 单次执行（ime/service.cpp:140-145），`s_classRegistered` 手工标志已移除。
- **L3** 未修复 → 已修复闭环（静态确证，防御性）。`GetString` 缓冲改 `(cb+1)/2` 向上取整（common/settings.cpp:97-99）。注明：`RegSetValueExW` 写 REG_SZ 做 4 字节对齐，经正常 API 无法构造奇数 cb，本修复属防御加固而非在来缺陷。
- **L5** 测试工具 → 已修复闭环（**已确证**）。engine_test stdout 切 UTF-16 文本模式（tools/engine_test/engine_test.cpp:146），汇总行不再截断。
- **L6** 未修复 → 已修复闭环（静态+链路确证，**待真机**）。`reg.exe` 纳入 Job Object、`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`（ui/settingsbackup.cpp:36-40），超时关作业对象即收割孙进程——旧实现只杀 cmd 自身会留 reg.exe 孤儿。
- **L7** 未修复 → 已修复闭环（静态+链路确证，**待真机**）。`FlushUserDict` 挂 `IPredictor` 虚口（ui/engine/predictor.h:49、pinyin.cpp:1528-1530），词典导出与设置备份读盘前经 `g_hooks.flushUserDict` 收敛在途学习（ui/userdictwnd.cpp:319-321、ui/settingsbackup.cpp:105-107）。
- **L8** 未修复 → 已修复闭环（静态+链路确证，**待真机**）。三管理窗越界不再静默假报"已删除"，改提示"列表已变化，请刷新后重选"（ui/phraseui.cpp:235-239、blockedui.cpp:194、hotphraseui.cpp:323）。
- **L9** 未修复 → 已修复闭环（静态确证）。usage.txt 格式化缓冲 256→512（ui/usagestats.cpp:158）。
- **L13** 未修复 → 已修复闭环（静态确证）。`ReadFileBytes` 判 `INVALID_FILE_SIZE` 并加 16MB 上限（ui/userdictwnd.cpp:492-496）。

### 7.2 本轮新修复（修复会话落地，本轮对码复核定位）

- **H3** 未修复 → 已修复（静态确证，**残余风险见 §八-1/-2**）。超时收敛原则统一为"等不到就保留锁/泄漏资源，而非拆锁 free"：① pipeserver::Stop 有界等四类线程（重排/客户端读等）归零再拆锁，超时不拆（ui/pipeserver.cpp:126-138），spawn 前先计数防起步放行（:216-220/:651）；② LlmPredictor 析构有界等 `_inflight`，超时不释放任何 llama 资源交进程退出（ui/engine/llm.cpp:234-239）；③ 推理入口 RAII 计数（:253/:264）；④ PinyinEngine 析构有界等后台落盘线程离开 `this`（ui/engine/pinyin.cpp:384-389）。新增 `WarmupAsync`：先计 `_inflight` 再 spawn（llm.cpp:280-283、:1067-1070），预热线程纳入析构等待窗口，堵住原 detached 预热裸逃逸。**（2026-11-03 复核：本条为第一批收敛修复；同日 `b8d1d8d` 进一步根治——main 域停机不析构 + KickPersist 去 this 化，升为闭环，见§7.5；§八-1/-2 销账。本条所引行号为审查时实测，后续提交后已漂移，现行号以§7.5 引用为准。）**
- **H4** 未修复 → 已修复（静态确证）。候选窗 5 个跨线程入口统一按 `_ownerTid` 判属主、非属主封包 PostMessage（WM_APP+30）投递属主线程串行落地（ui/candwnd_core.cpp:225/264/294/339/390，属主记录 :52，封包定义 :21/405-423）；detached 重排线程改走 `PostShow`（ui/pipeserver.cpp:707）。pipeserver 调用点零改动（封包逻辑全部内聚在窗口层）。
- **M2** 未修复 → 已修复（静态确证）。非法/超长 ns 不再静默回退真实键：显式报错一次 + 降级固定隔离键（common/settings.cpp:13/45-46），与 `TestSpawnedFromEnv` 同口径（:114-124），消除"自认沙盒实写真实注册表"分裂。
- **M5** 未修复 → 已修复（**实测确证**）。击键热路径改 `SettingsSeq` 版本号 + 500ms TTL 缓存（common/settings.cpp:83-87/148-174）：本进程保存侧写后 bump seq 即时失效（:294/:355），跨进程读侧 seq 变更即失效；TTL 兜底外部直写。**未采用注册表 LastWriteTime——其粒度为秒，毫秒级改动不可见**。实测热路径 640.79µs → 21.67µs（约 30 倍）。残余陈旧窗口见 §八-3。
- **L4** 未修复 → 已修复（静态确证）。`DllCanUnloadNow` 增 `g_cThreadRefs` 判定（ime/dllmain.cpp:21/93）；pipeclient 在 `CreateThread` **前**持引用、失败即回退、`Run` 退出归还（ime/pipeclient.cpp:15-20/48），覆盖"线程已创建未运行"窗口期。

### 7.3 对码补记：代码已核实修复、任务简报未逐条列出者

> 以下条目修复会话简报未点名，本轮逐文件只读核验发现代码已带对应修复标签，按"与实际代码对齐"原则更新状态；如与修复会话认知不符请回补验证。

- **M1** 未修复 → 已修复（静态确证）。编辑会话任务自持 AddRef 快照，不再读调用方尾部可能已释放的成员（ime/composition.cpp:525-526）。
- **M3** 未修复 → 已修复（静态确证）。`Stop` 检查等待结果，线程 3s 未退则保留 `_cs/_cb` 不拆不删（宁泄漏不 UAF）（ime/pipeclient.cpp:28-38）——与 H3 同一收敛原则。
- **M4** 待复现（疑似）→ 已修复（静态确证）。失焦 else 分支补 `ClearCandidates()`、与 `OnKillThreadFocus` 同口径（ime/service.cpp:295-297）。原缺陷始终未复现，本条属同口径加固。
- **M6** 待复现（疑似）→ 已修复（静态确证）。双防御：`IntToCn` 入口全量 isdigit 校验（ui/engine/calcnum.cpp:32）+ `FormatDouble` 显式 C locale（:140），不再依赖 setlocale 前提。
- **M7** 待复现（疑似）→ 已修复（静态确证）。BuildIndexes 空音节守卫（ui/engine/pinyin.cpp:848）。
- **L11** 待复现（疑似）→ 已修复（静态确证，**待复探针**）。括号与一元运算符共用深度闸 64（ui/engine/calcnum.cpp:195-208/217），注释明确"一元同为裸递归须共闸，语义不变"。**注意**：任务简报称"一元负号残余未修（20 万连发 0xC00000FD）"，但当前代码一元闸已落地——可能系简报之后落地，须重跑 20 万连发探针确证后方可闭环（§八-5）。

### 7.4 仍有缺口/未修

> **2026-11-03 复核：本节两条均已由后续批次修复落地，"未修"清单清零。** 原文留档：

- **L10** ~~设计取舍（维持未修）~~ → **已修复（`53f0aaf`，见§7.5）**。原记"键数仍无上限、未加封顶"已失效——键数封顶 2 万+批量淘汰+manual 豁免已落地。
- **L12** ~~待复现（维持未修）~~ → **已修复（`bda5094`，见§7.5）**。原记"代码无修复标签"已失效——重建路径清 `g_toggleOn`+释放旧字体 GDI 句柄已落地。

### 7.5 复核增补（2026-11-03，账本收尾轮）

> 本轮将总表全部 26 条状态与 HEAD=`b8d1d8d` 代码逐条对码复核：§7.1-§7.3 所引修复标记/守卫逐条 grep+读码命中（H1 pipe_util.cpp:96-109、H2 pipeserver.cpp:738/791/809 sendMtx、H4 candwnd_core.cpp:225-390 `_ownerTid` 五入口+PostShow、H5 llm.cpp:270、M1 composition.cpp:525-549、M2 settings.cpp:13/45-46、M3 pipeclient.cpp:28-38、M4 service.cpp 失焦分支、M5 SettingsSeq+TTL、M6 calcnum.cpp:32/140、M7 pinyin.cpp:873、L1 magic-static、L2 InitOnce、L3 settings.cpp:99、L4 dllmain.cpp:93、L6 Job Object、L7 flushUserDict 链路、L8 三窗提示、L9 512、L13 INVALID_FILE_SIZE+16MB 上限）。**未发现"文档声称已修、代码未修"的反向冲突**；发现三处反向滞后（代码已修、文档未同步），更正如下。

- **H3** 已修复 → **已修复闭环（台架实测确证，`b8d1d8d`）**。根治=组合方案：① main 域"停机不析构"——PipeServer/CandWindow/引擎全部堆上生、退出故意不 delete（ui/main.cpp:519/533 堆分配，:636-647 停机尾部无 delete、退出前显式 `FlushUserDict()` 收敛最终落盘），僵尸线程能触的内存恒有效、由 OS 统一回收；lambda 全部按值捕获指针，消除 `[&]` 栈变量悬垂。② KickPersist 落盘线程**去 this 化**——快照在调用线程持锁完成，线程只持按值 content/path + `shared_ptr<PersistCtl>`（busy/dirty/ownerDead，ui/engine/pinyin.cpp:736-742/:1531-1552）；析构墙钟 5s 等 busy 超时后置 ownerDead（:385-392），在途僵尸线程写前检查、放弃写盘——超时后亦无 UAF。③ 有界等待全部改墙钟 deadline（`GetTickCount64`）：旧"2000×Sleep(1)/5000×Sleep(1)"因 Sleep(1) 实际粒度约 13ms，名义 2s/5s 实跑 27s/65s（PITFALLS #94）。**验证**：工装 A 17/0（20500 键大词典+高频 Learn+立即析构 4 轮，userdict.txt 逐行严格校验无清空/半截——L10 轮"重启后文件疑似被旧线程清空覆盖"现象未复现，其根因〔线程触已析构对象序列化出垃圾〕已被②结构性消除）；工装 B 3/3（重排/续写在飞时经新增 `Local\LingxiUI_Exit` 触发口走真实停机路径，退出码 0、无 WER dump）；工装 C 4/0（落盘目标不可写时 flush 墙钟 2s 有界返回、析构 <6s、无 tmp 残留泄漏）；kill/verify（ExitProcess 腰斩在写线程，主文件恒为完整旧态/新态——WriteFileAtomicA 原子写契约）。门禁：Release/Debug x64 零错误零警告、engine_test 279/0、fix_readings 缺口 0。残余风险改登记 §八-6/-7。
- **L10** 设计取舍（未修）→ **已修复（静态确证 + 工装 A 淘汰参与，`53f0aaf`）**。`_userDict` 键数封顶 2 万（ui/engine/pinyin.cpp:726 `kMaxUserDictKeys`），越限时按"键内条目总频次最小"批量淘汰 5%=1000 键（:1415-1463 `EvictUserDictKeysLocked`，仅新键插入前触发，既有键计数更新不触发）；manual（用户显式添加/导入）词条持久化第 4 列、豁免淘汰（:1276/:1503/:1695/:1750）；全 manual 越限不淘汰（封顶只约束自学习通道增长）。**验证缺口如实记录**：淘汰受害者选择策略无 engine_test 单测断言；运行时证据=工装 A 20500 键压测中淘汰真实参与且文件完整性严格校验通过；长期增长观察仍按真机剧本 7（manual-verify §七 7c 判据现已适用）。
- **L12** 未修复（待复现）→ **已修复（静态确证，原缺陷始终未复现，`bda5094`）**。设置窗重建路径先清 `g_toggleOn` 陈旧 HWND 键表（ui/settingsui.cpp:953-956，防同值句柄复用命中陈旧开关态+残留键无界泄漏），六支全局字体经 `setFont` 覆盖前 `DeleteObject` 旧 GDI 句柄（:1052-1058）。与 M4 同口径：属同型加固，非复现后修复；原触发场景（外部 DestroyWindow）仍无用户可达路径，不安排真机动作。

## 八、已知残余风险

1. ~~**H3 根治未做（结构性）**~~ **已销账（2026-11-03 复核：`b8d1d8d` 已做根治，见§7.5）**。原文留档：本轮修复策略是"超时即保留锁/泄漏 llama 资源"，宁泄漏不拆锁；但僵尸线程仍可能触碰 ui/main.cpp 中已 `delete` 的 `server`/`wnd` 对象内存（main.cpp:626-627 `delete engine; delete pinyinEngine;` 仍在）。根治需 main 域改"停机不析构"，属结构性改动，**未做**。→ 现状：main 域已改"停机不析构"（对象堆上生、退出故意不 delete），delete 全部移除，僵尸线程可触内存恒有效；pipeserver Stop 超时保留锁由"权宜"升级为完整收敛契约的一部分（ui/pipeserver.cpp Stop() 注释）。DLL 侧同类窄窗口转登 -6。
2. ~~**KickPersist 等待上限 2s→5s**（ui/engine/pinyin.cpp:389）：有界等待放宽后，磁盘挂死时退出路径理论上仍有 UAF 窗口（上限 5s，比旧实现概率更低但非零）~~ **已销账（2026-11-03 复核：`b8d1d8d` 改线程模型，见§7.5）**。→ 现状：落盘线程不再触引擎内存（只触 `shared_ptr<PersistCtl>` 状态块+按值快照），超时后**无 UAF**；ownerDead 标志防僵尸旧快照覆盖退出期最终落盘；等待改墙钟 deadline（名义 2s 实跑 27s 的 Sleep 粒度缺陷一并修复，PITFALLS #94）。退出耐久度取舍转登 -7。
3. **M5 缓存陈旧窗口**：500ms TTL 仅对绕过 `SaveSettings` 直写注册表的外部工具（reg.exe 等）兜底——其变更最长 500ms 不可见；`deep_test` 毫秒级断言未验证过该窗口。
4. **L6/L7/L8 行为验证依赖真机**：本轮屏幕控制禁用，仅静态+链路确证；待真机验证项——备份超时时孙进程收割、导出时在途学习收敛、越界删除的提示路径。
5. **L11 待复探针**：一元负号深度闸代码已落地（§7.3），20 万连发 `0xC00000FD` 探针须在落地后重跑确证；未确证前不得按"已闭环"对待。**（2026-11-03 复核注：一元闸仍在码〔calcnum.cpp:161/195-197/215，`ec376d8`〕；DEVLOG 与工装记录中未见复探针确证，维持"待复探针"，不升闭环。）**
6. **pipeclient（DLL 侧）窄窗口 UAF——有意维持现状（2026-11-03 登记，非遗漏）**：`CPipeClient` 是 `CTextService`（COM 对象）的成员，宿主 `Release()` 引用归零 `delete this` 与 `Stop()` 3s 超时僵尸线程（仍触 `_cs`/`_cb`/`this`）之间存在竞态窄窗（ime/pipeclient.cpp:22-38）。**缓解已就位**：① M3 超时路径保留 `_cs`/`_cb` 不拆不删（宁泄漏不 UAF 崩宿主）；② L4 `g_cThreadRefs` 使 `DllCanUnloadNow` 在线程在途时保守拒载，DLL 不会被 FreeLibrary 抽走线程栈底代码；③ 3s 收敛在正常宿主必达——走到超时分支说明宿主已病态，此时泄漏无实质影响。**不做根治的理由**：彻底消窗需把回调改 weak/COM AddRef 生命周期语义，属 DLL/COM 生命周期重构，且只能在真机注册 TIP 的宿主进程内验证（deep_test/e2e 域）——成本/收益不匹配。**真机验证口**：manual-verify 剧本 2（L4+M3）。
7. **退出落盘耐久度取舍（2026-11-03 登记）**：停机尾部 `FlushUserDict` 墙钟有界等 2s（ui/engine/pinyin.cpp:1614-1626），短于旧析构路径的名义 5s。判据：Learn 每次都 KickPersist，退出时词典几乎必已落盘，2s 只收最后尾差；磁盘挂死 >2s 则放行退出，文件停在上一完整态（WriteFileAtomicA tmp+FlushFileBuffers+原子 rename 保证任何腰斩点都不半截，kill/verify 实测）。代价=极端场景最后一次学习可能丢，收益=退出路径耗时有硬上界（旧计数法名义 5s 实为 ~65s）。
