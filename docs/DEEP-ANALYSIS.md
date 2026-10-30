# 深度分析 · 2026-10-30 晚（光标停滞修复后全项目扫描）

> 触发点：用户真机报障"记事本中上屏文字后光标停留原位"。本轮完成根因修复
> （`8b2ba79` 之后的 fix(ime) 提交）后，对全项目做同类风险面 / 测试盲区 /
> 架构债的系统扫描。证据均来自本轮代码检索与 A/B 实验，非回忆性总结。

## 0. 本轮缺陷复盘（一段话）

TSF 规范对 `ITfInsertAtSelection::InsertTextAtSelection` 之后的选区/光标位置
**零承诺**（MSDN 只定义插入位置与返回 range，选区是否前进完全由宿主 store
实现决定）。灵犀 DLL 从不调用 `SetSelection`；全部测试宿主（deep 旧版/e2e/
host32）却在插入后"好心"自推选区 → 测试全绿；Win11 记事本（商店版，WinUI
CoreText TSF 适配层）严格不动选区 → 上屏后光标停原位、后续输入插到旧光标处。
修复=提交后显式 `SetSelection` 收敛宿主差异（宽松宿主同位幂等，严格宿主补上
前进），并以 deep 严格 store（模拟记事本）+ T19 光标契约用例钉死。

证据链（in-vitro 复现，三轮确定）：

| 面 | DLL | deep 严格 store 结果 |
|---|---|---|
| A | 修复前 | T14/T18a/T18b/T19a/T19b 五红（"好你"顺序错乱、联想作废、sel={0,0}） |
| B | 修复后 | 35/35 全绿（T19a sel={2,2}、T19b sel={3,3} 前缀保持） |

## 1. 同类风险面扫描：TSF 契约假设（本轮修复的"类"）

DLL 内全部选区交互点（grep 证据）：

| 位置 | 用途 | 严格宿主下行为 |
|---|---|---|
| `ime/editsession.cpp` CInsertSession/CReplaceSession | 上屏/替换写入 | **本轮已修**（显式 SetSelection 推光标） |
| `ime/composition.cpp:69` | 读会话插入点探测 | 读当前选区，不写——选区正确性由宿主+本轮修复共同保证 |
| `ime/composition.cpp:317` / `:686` | 联想/续写 pre-caret 上下文读取 | **修复前在严格宿主下读空**（T18a 作废的机制）；修复后选区正确 |
| `ime/service.cpp:288` | Chromium 异步自插入 OnEndEdit 抑制（400ms 窗口） | 与新 SetSelection 同一编辑会话内，OnEndEdit 只触发一次，无新增事件——无回归 |

结论：**选区契约面已闭环**。中文提交（InsertTextAtSelection）、英文补全
（ReplaceTextBeforeCaret）、标点/v 悬账路径全部经这两个 session，无旁路
（grep CInsertSession/CReplaceSession 仅 service.cpp:615/:632 两个构造点）。

残留同类风险：
- **死代码中的契约**：内联组合路径（`_inlineComp` 硬编码 false，service.h:236，
  全库无置 true 点）含 `ITfContextComposition` 组合范围操作，从未被任何测试
  覆盖、也未经本轮契约审查。若未来复活，需按同等严格度重审（见 §3 P2）。

## 2. 测试矩阵与盲区

当前宿主风味覆盖：

| 套件 | store 行为 | 对应真实宿主 | 状态 |
|---|---|---|---|
| deep_test | **严格**（本轮起：文本变更不动选区，GetSelection 钳制，宿主侧 ClearFocusedText 重置选区） | Win11 记事本 / WinUI CoreText | 35 断言全绿含 T19 契约 |
| e2e | 宽松（插入自推选区） | 传统 EDIT 控件类宿主 | 全绿（attempt 1） |
| host32 | 宽松（手动探针工具） | — | 不进 regress |

本轮确立的设计原则：**光标前进只信 DLL 的显式 SetSelection，不信宿主好心**。
T19 是该契约的常驻守卫（顺序+前缀+选区三元断言）。

盲区与已知噪声：
1. **真实记事本无自动化**——严格 store 是代理建模，无法替代真机人工抽测。
   建议：每次涉及上屏路径的改动合入后，人工在记事本打一句话（顺序+光标+
   连续两次上屏），两分钟成本。
2. **机器负载噪声窗口**（今晚实证）：19:47-58 e2e 六连"identical 假红"
   （pass-through 读空），安静环境同二进制复跑直接绿。regress 的 3-attempt
   政策能吸收偶发，但**窗口期噪声会连杀 3 attempt 且症状同一**——forensics
   纪律见 §4。
3. **TestStore 三处重复**（deep/e2e/host32 同源骨架）：本轮起 deep 与 e2e
   **有意分叉**（严格 vs 宽松）。重复代码的同步成本已用注释互相锚定
   （deep 注明"模拟记事本"、e2e 注明宽松），后续改任一 store 需检查另一处
   是否需要跟进（PITFALLS 候选）。

## 3. 架构与工程债清单（按优先级）

- **P2 内联组合死代码**：`composition.cpp` 的 `!_inlineComp` 分支群 +
  PinyinEndComposition/Append/Start 等。约数百行，无测试覆盖、无调用路径。
  建议下轮专项删除（减维护面、消除 §1 残留风险的载体）；删除前跑全量
  regress 确认零行为差。
- **P2 LINGXI_LOG 默认关 → 零 in-vivo 证据**：本轮报障无真机日志可用，
  只能 in-vitro 复现（成本约 40 分钟实验）。建议：保持 env 门控（日志毁
  延迟，PITFALLS #20），但在 HANDOFF 固化"报障取证 SOP"：先开
  `LINGXI_LOG=1` 复现一次再报，或考虑 DLL 侧 8KB 内存 ring buffer +
  崩溃/手动触发落盘（P3 级想法）。
- **P3 用户字典双写与静默丢失**（QC-REPORT 2026-10-27 立项）：设置界面
  WriteFileBytes 与引擎 SaveUserDict 并发写同一 userdict.txt；设置 UI
  工作（settingsui）合入后**需复查是否已闭环**，未闭环则按 QC 建议
  （临时文件+MoveFileEx 或统一经引擎锁）。
- **P3 UserDictPath 双实现**（设置侧 vs 引擎侧路径拼接重复）——路径漂移
  风险，收敛到 common。
- **P3 e2e 直通断言前的 fail-fast**：本轮已把 [DIAG] 诊断挪到判定前
  （e2e.cpp），失败现场永久可见——同类"失败路径无现场"的断言点建议
  后续照此办理。

## 4. 运维取证教训（今晚 e2e 假红事件复盘）

时间线：19:47-52 regress e2e 三连败 → 19:54 换 DLL(19:46 版) PASS →
19:56 换回(19:48 版) FAIL → 20:04 再换(19:46 版) PASS → 20:12 第三版
PASS → 20:16 **19:48 版安静环境复跑 PASS**——假红实锤为环境噪声窗口。

方法论教训（已立 PITFALLS）：
1. **A/B/A 三样本陷阱**：三次交替实验里 1 次巧合即可制造"完美反转"假象；
   同源二进制（7 字节 PE 元数据差异）行为分叉=物理不可能，矛盾即证伪。
2. **二进制对比是悖论终结者**：`Get-FileHash`+逐字节 diff（本轮 7 字节全在
   PE 头/.rdata 时间戳区）可在几分钟内排除"构建引入差异"假设。
3. **安静环境复现是金标准**：负载窗口期（本机跑测试/杀毒扫描/后台任务）
   的连败不可作为二进制判定依据；复跑一次安静环境版本再下结论。
4. `build.cmd` 的 `.prev` 轮转保留源 mtime（PowerShell copy 语义），
   随机后缀+轮转上限——取证时以 mtime+hash 定身份，勿信文件名。

## 5. 建议与优先级（下一轮候选）

1. 内联组合死代码删除专项（P2，先 regress 零差再删）。
2. 用户字典双写闭环复查（P3→若未闭环升 P1，数据丢失类）。
3. 记事本人工抽测 SOP 固化进 HANDOFF（零成本高保真）。
4. TestStore 同步守则：改任一 store 行为时，检查另一 store 是否有意分叉
   （写入两处注释互链，已做）。
