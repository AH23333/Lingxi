# 交接文档（HANDOFF）

## ⚡ 最新状态速报（2026-10-29 下午·新会话：AI 补全大量重复修复 `d5a7d64` + regress 全绿）

- **用户定性**：AI 补全词大量重复（大问题）+ 候选词需优化 + 设置页各方面差。本轮收重复（high），后两项排队。
- **`--probe-typing` 取证**（ui/main.cpp 常驻探针，逐字中文+逐词英文过真实链）：三种重复形态实证——归一相等对（散心/散心。）、前缀堆叠对（晴，气温20℃-30℃/…湿度70%-80%。）、0.5B 整段复读上文（ctx"…公园"→"天，今天天气真不错"）；另 LLM 空时漏出 M3 词频占位垃圾（了是在和有他不我的）。
- **三重去重 `d5a7d64`**：ngram.cpp 新 `DedupCompletions(cands, ctx)`（predictor.h 导出，llm.cpp Generate 合并后统一过闸）——①归一相等保首 ②互为前缀保较短（≥2 字才并） ③与上文二元组重叠 ≥60% 剔；**WordEngine::PredictChinese 占位退役**（中文续写 LLM 独占，无候选>垃圾）。probe 复跑三形态全灭，英文词表补全无恙；代价=部分步 0 候选（原全垃圾）。
- **测试基建两修**（Loomy 改动+退役叠加暴露）：deep_test T14 改**扫描式选词**（序 pin 双重状态依赖——29f9ec2 质量闸改序只同步了 engine_test；深测规范序 你@idx5 与开发机探针序差一位，开发机 userdict 有历史测试污染）；e2e test5 中文 Tab 预算 3s→12s（LLM 冷启动+负载超旧预算）。
- **regress ALL PASS**（engine 186 断言含 E8×9、deep 扫描式 T14、e2e attempt 1；latency 首跑红=Qoder 后台噪音，同 binary 空闲 8.9ms 过）。真实 UI 未重启（用户会话实例勿 kill——新 exe 待用户空闲时随构建生效）。
- **下一优先级**：①候选词优化（排序/视觉/操作/污染，用户四项全选）②设置页重做（视觉/组织/交互/尺寸，四项全选，无风格偏好）——两项开工前先向用户取证具体期望。

（以下为历史速报。）
## ⚡ 最新状态速报（2026-10-29 下午·Loomy 走查收口：五感投诉处置 + 自走查四修 + 文档全量同步）

- **用户五投诉处置完毕**（各带提交）：候选窗黑描边/屏幕残影（CS_DROPSHADOW 方影+DWM 边框线+toast 裸隐藏+异步首帧四因分修 `b206e42`）、空输入弹预测遮屏（锚定 caret→鼠标→记忆位三级+双轴钳制 `b206e42`）、设置页滚动频闪（WS_EX_COMPOSITED `d8c7db1`）、续写"全是同一句话的部分重复"（SplitContinuation 去前缀堆叠→短句+整句≤2 条 `dfc9ec5`）、英文上下文混中文建议+缩写过拟合（语言闸+空文闸+单字符缩写禁止 `dfc9ec5`）。
- **自走查轮**（用户令"自己测试，不外包"）：pf_shot 截图回路抓到 变体刷屏（词长偏好+cost 上限双闸 `29f9ec2`）、回显掐错头+漏选字体+构建戳外泄（`6536e3c`）、设置整页回写刷零放大器（loaded 门+SaveSettingsDiff `04ac2fb`，PITFALLS #44）；「--settings 不弹窗」结案为跨会话观测假象（#45），转发链补观测日志（f4911ff）。
- **数字**：engine_test **176/0**（新增变体闸断言）；真实 HKCU 设置已恢复正确默认；用户会话 UI 实例由宿主自动拉起（勿 kill 抢位）。
- **待验证两项**（同会话复现即可判定）：①设置页底部裁切/滚动条真值；②owner-draw 勾选框 checked 态截图复核。
- **文档面**：README 全量重写（清除"默认开/x64 仅/142 项"等矛盾与过时口径，补隔音/滚轮/部分选词/回显/快捷短语/皮肤/32 位/卸载 purge）；PROGRESS 同步本轮行。

（以下为历史速报。）
## ⚡ 最新状态速报（2026-10-29 晚·十四·续，空闲全量回归收口 + B6 eat 侧显式接线 + T17 端到端）

- **晚·十三/十四审读债全清**：用户停机后 `tools\regress.cmd` 全量（tmp\regress-1338211878）：engine_test **latency 7.156ms**（安静基线恢复，PITFALLS #41 归因闭环）、fix_readings、deep_test 26/26 AV=0、e2e attempt 1 全 PASS。
- **B6 eat 侧断线修复（`ime/composition.cpp`）**：组合中 VK_OEM_7 吃键此前依赖 cnPunct 标点映射间接达成——**cnPunct=false 时撇号穿透宿主，B6 handler 死码**（晚·十三"误提交首选"推断有误，实为穿透）。PinyinKeyEaten 显式 case：无 Shift 且非双拼直接吃，与 handler 闸一致，不依赖 cnPunct；Shift+' 引号/双拼路径不变。
- **deep_test T17（+3）**：导航键 6 键组合期全吃下组合不丢 / 空组合负对照放行 / **xi'an 空格提交西安端到端（0x897F,0x5B89）**。deep_test attempt 1 即 **29/29 PASS AV=0**。
- **probe 实探**：xi'an→西安 rank 0（45 cands 无"先"）；xian→先 rank 0、西安 rank 9——隔音因果对照成立。probe 输入 xi'an/xian 已常驻 ui/main.cpp。
- **验证口径**：最终 regress deep_test/e2e/fix_readings PASS；engine_test 174 功能断言全绿（separator 6/6），唯一红 latency 20.6ms=**电池供电降频**（PowerLine Offline，词典解析 6.9s→15s 佐证；插电基线 7.2ms；期间并行 StarMark dotnet test 干扰已排查排除）——用户裁定接受电池模式结果，功能全绿。真实 UI 已重启（PID 7656，晚于构建，新 DLL 生效）。
- **待做（下一轮首要）**：**UI 视觉/体验重做——用户 2026-10-29 定性"依旧很差"**。晚·十二 WinUI 化方案未达预期，下一轮不要在原方案上小修，开工先向用户取证具体差项（配色/布局/字号/间距/交互哪几处、有无参考样式或截图），按取证结果重做；电量/时间允许时配合 UI 走查截图逐窗确认。
- **待做（其余）**：R3 压测收尾（真机 p50/p95）；组合光标编辑（M5 尾巴，Left/Right 已预留吞下）。（原"UI 视觉人工确认"已被上面的重做任务取代）

## ⚡ 最新状态速报（2026-10-28 晚·十四，导航键翻页与组合期接管 + strictSep 撞键校验修正）

- **strictSep 修正**：上轮 compact 音节数校验误拒无隔音输入的合法串级匹配（xian 的"西安"走 [xian] 路径 tier-0 被拒成 tier-1 截断丢失——新增 no-over-reject 断言抓到）。改为 Predict 按 NormalizeIn 口径（弯撇号同算、双拼转换后计算）算 `strictSep` 传入 MatchWords：有隔音才紧校验（xi'an 不出"先"），无隔音串级匹配是合法读法。
- **组合期导航键接管**：PinyinActive() 时 Up/Down/Home/End/Left/Right 一律吃下（此前穿透宿主→光标移出组合范围→SetText 错位，B5 同族）；Up/Down 翻页、Home/End 首末页（新 `PageCandidatesHomeEnd`）、Left/Right 吞下保守组合（组合光标=M5 尾巴预留）。Ctrl/Alt 组合直通不受影响。
- **P6e +2**：`'an` 安 cover=3（B6×B1：隔音边界随后续音节消耗不留死撇号）；xian 西安 cover=4（回归断言）。
- **验证**：engine_test separator 6/6 全绿；latency 25.4ms=环境漂移（left4dead2，PITFALLS #41）；build.cmd 全链绿。**DLL 导航键路由仅代码审读**（deep_test 用户用机中未跑）；**真实 UI 未重启**（新 DLL 待重启应用生效）。
- **待做**：用户空闲后 regress 一键回归 + 真实 UI 重启 + xi'an/导航键/滚轮真机验证 + UI 视觉人工确认（晚·十二遗留一并）；R3 压测收尾；组合光标编辑（M5 尾巴，Left/Right 已预留吞下）。

## ⚡ 最新状态速报（2026-10-28 晚·十三，B6 拼音隔音符 + B7 候选窗滚轮翻页——拓展轮）

- **B6 拼音隔音符**：组合中 `'`（VK_OEM_7 无 Shift）进组合当切分边界（xi'an→西安；此前落 unmapped-default **误提交首选**）；根因修复 MatchWords 精确 compact/模糊音分支补音节数校验（[xi][an] 拼接 compact="xian" 撞单音节词"先"——PARTIAL 分支有校验另两处漏）；NormalizeIn 弯撇号改 UTF-8 三字节序列探测（旧 `'\u2019'` 窄字面量与单字节 char 比较恒假死代码）。双拼/Shift+' 引号/空组合行为不变。engine_test P6e +4 断言全绿。
- **B7 滚轮翻页**：候选窗 WM_MOUSEWHEEL 翻页（±kPageSize 夹取；Win10+ 悬停滚动路由；键盘 PageUp/PageDown、-/= 不变）。纯 UI。
- **文档债**：PROGRESS M4.5 KV 增量 ⬜→✅（`8b5ed3b` 早已实现，旧行过时）；settings.h skinIndex 注释对齐五皮肤实际。
- **验证**：engine_test 沙盒功能全绿；唯一红 latency 14.3ms = left4dead2+deveco 后台漂移（PITFALLS #41 归因口径，安静基线 ~7ms）。build.cmd 全链绿（C4267 已修）。**deep_test/e2e 未跑**（用户用机中）——DLL 键路由 + UI 滚轮留空闲回归；**真实 UI 未重启**（新 DLL 待重启应用生效）。
- **待做**：用户空闲后 regress 一键回归 + 真实 UI 重启 + UI 视觉人工确认（晚·十二遗留一并）；R3 压测收尾（真机 p50/p95 对标 ≤450ms）。（卸载清理已随 R5 完成——unregister.cmd --purge 实证；代码签名无证书不可行，已知限制）

## ⚡ 最新状态速报（2026-10-28 晚·十二，全应用 WinUI 风格重做——去蓝紫收口）

- **UI WinUI 化全量完成（`bc7be9e`）**：新共享 `ui/uistyle.h` 设计令牌——中性灰阶八常量（kUiBg/kUiCard/kUiCardBorder/kUiFlyoutBg/kUiFlyoutBorder/kUiText/kUiText2/kUiControlEdge）+ 非蓝紫五皮肤（0 翠绿默认/1 青碧/2 橙/3 红/4 金，Settings.skinIndex 全 UI 统一换肤）+ `UiApplyRoundCorners`（Win11 DWMWCP_ROUND 优先一次登记、CreateRoundRectRgn 回退随尺寸重设）。候选窗 DWM 圆角+CS_DROPSHADOW+圆角行高亮（选中 accent 白字/hover hoverLight）+组合回显去蓝；设置页白卡化（SECTION 开合卡片+RoundRect 16 描边+FW_SEMIBOLD 段标题+皮肤色 owner-draw 勾选框/按钮+WM_CTLCOLOR 全套+滚动全量重绘防残影）；符号面板圆角浮层+药丸分类；词典窗/快捷短语窗统一中性底+雅黑。
- **验证**：tools\build.cmd 全绿（四目标无 error/warning）；窗体视觉无自动化断言（B2 同口径），用户用机中未跑 UI 验证——**留用户空闲时人工确认各窗口视觉**，如需微调再小提交。
- **编译坑**：settingsui.cpp owner-draw 画刷（g_hBgBrush/g_hCardBrush/g_hHint）初版声明在布局块而 UserDict 窗先用→C2065，移文件顶 globals 修复；docs/源码批量替换沿用 PITFALLS #36/#40B（必 edit 工具+唯一锚）。
- **待做**：UI 视觉人工确认（用户空闲后）；R3 压测收尾（真机 p50/p95 对标 ≤450ms）；卸载清理/代码签名（M6 尾巴）。

## ⚡ 最新状态速报（2026-10-28 晚·十一，F AI 预测=R3 多路采样收官——用户六项需求 6/6 全完成）

- **R3 定性纠正 + 多路采样落地（`b3bad05`）**：旧文"R3 未实施"过时——核心（DLL 补发上文 `e328a7d` / 双上下文 KV 隔离+prefill 硬闸+EMA 降级 `4ca0218` / 重排默认关 `684d22a`）前几轮已落地；本轮补最后一块 §3 多路采样：LlamaApi 绑定 10 个 `llama_sampler_*`（全可选，缺则采样路禁用贪心路无恙）+ 续写 fork 采样合并候选（KV 高命中且贪心 ≤60ms 才触发，seq_rm 到末位前一格重 decode 末位取新鲜 logits——直接用残留 logits 会采错位置分布）+ 拼音重排 Borda 投票（并列取贪心序，**top-1 不一致回退原序**）+ seed=FNV-1a(prompt) 防候选闪烁。HKCU `LlmMultiPath` 默认关（+150ms CPU/次实验特性），设置页智能特性区勾选框（IDC 119）。
- **用户六项需求 A-F 全部完成**（A `7a28923`/D `7065b2a`/B+E `ebf0ae1`/C `7bf02f5`/F `b3bad05`=R3 全量）。
- **验证**：最终二进制 engine_test（时延 7.07ms 空载窗口）/fix_readings/deep_test 26/26（AV=0）/e2e 全绿；**时序项归因方法**（PITFALLS #41/#42）：当晚 left4dead2 后台 + 残留 LingxiUI 的 LLM 预热（~45s CPU）使 10ms 预算漂到 14ms，stash 旧码 A/B 同漂=环境非代码；测试前必 `taskkill /f /im LingxiUI.exe`。
- **待做**：R3 压测收尾（从 UI 日志提预测 p50/p95 对标 ≤450ms——需真机开 LlmRerank/LlmMultiPath 实测）；皮肤/卸载清理/代码签名（M6 尾巴）。

## ⚡ 最新状态速报（2026-10-28 晚·十，用户六项需求 A/D/B(含E)/C 四单元落地——F AI 预测=R3 留下轮）

- **A 自定义中英切换键（`7a28923`）**：toggleMode 扩五态 0关/1 Shift/2 Shift或Ctrl/3 CapsLock/4 Ctrl；CapsLock 三态状态机（OnTestKeyDown 即吞防大小写翻转，KeyUp 干净单击切换）；Shift/Ctrl 组合判定改精确枚举；设置页五态下拉。
- **D 快捷键句库（`7065b2a`）**：`ui/hotphraseui` 新模块——全局热键一键上屏句子（%APPDATA% 存储，上限 24 条，热键强制 Ctrl/Alt，管理窗热键录制框）；RegisterHotKey id=100 起，插入复用符号面板 MSG_INSERT_TEXT 链路，**DLL 零改动**。
- **B/E 自定义候选词与词典增强（`ebf0ae1`）**：用户词典窗手动添加词条（拼音留空自动推断——zh_pinyin.txt 整词优先/单字回退）+ 提升/降低选中控排序（count±1000，引擎置顶区口径零改动）；520x480 重排。
- **C 设置页可缩放美化（`7bf02f5`）**：流式重排布局引擎（加宽自动折多列）+ 纵向滚动（内容超出动态挂/摘 WS_VSCROLL，解"min 高度依赖宽度"死结）+ WS_THICKFRAME 拖拽 + 浅底配色；探针烟测全绿（滚动 Δ=90 精确、双列折行、贴底、滚动条自动消失）。
- **deep_test T16 加固（`c00e972`）**：T16 前置 PinToggleMode(1)+结束还原——免疫环境设置页烟测残留污染（本轮实证 ToggleMode=4 残留致 T16 三连挂）；DLL 每事件实时读 HKCU，写即生效。
- **regress ALL PASS**；真实 UI 已重启（PID 25320，晚于构建）。PITFALLS 新增 #36-#40（PS 编码损坏源码/FindWindowW $null 封送/冷加载触发时序/探针插值假绿/环境依赖 pin+edit 多匹配）。
- **剩余**：F AI 预测 = R3 LLM 整句重排（方案 cur/GLM_EXPLORATION_R3_LLM.md，前置堵点：拼音模式 DLL 补发光标前上文；LlmRerank 已有续写路径可复用——RerankSettleMs=400/RerankBudgetMs=900 两阶段）。

## ⚡ 最新状态速报（2026-10-28 晚·九，32 位 TIP 落地——R5 工程化整节完成）

- **32 位 TIP（WOW64）全链完成（`88b9252`）**：ImeCore/common 加 Win32 配置（输出 `bin\<C>32\`，x64 零变化）→ `SysWOW64\regsvr32` 注册进 WOW6432Node 与 x64 共存（本机已注册，WOW6432Node CTF TIP Enable=1 实证）→ EnsureUIRunning 回退链（32 位 DLL 目录无 x64 UI 时回退 `..\Debug\`，真机实证杀 UI 后 `[pipe] 已拉起 LingxiUI.exe（32 位回退路径）` 自愈）。真机三环取证（tools\host32.cpp 探针留存）：加载 `bin\Debug32\ImeCore.dll dir32=1` / 管道已连接 / 回退自愈。**弯路**：Win11 24H2 `SysWOW64\notepad.exe` 是商店应用跳板存根（真身 64 位），探索文建议的验证路径不成立——自建 32 位文本宿主才有效；**TIP 实例化必须有焦点上下文**（窗口+ThreadMgr+TestStore+AssociateFocus），裸 ActivateProfile 不加载 DLL。regress ALL PASS，真实 UI 已重启（PID 9628）。
- **R5 工程化整节 ✅**：32 位 TIP + WER LocalDumps（本机已配，崩溃自动落全量 dump）+ SignalNamedEvent P2 + 卸载增强 + regress 常备。**剩余大项仅 R3 LLM**（方案就绪未实施，cur/GLM_EXPLORATION_R3_LLM.md，前置堵点：拼音模式 DLL 补发上文）。
- **本机 32 位注册是活跃状态变更**：32 位应用（WOW64 宿主）现会加载 `bin\Debug32\ImeCore.dll` 使用灵犀；卸载走 `tools\unregister.cmd`（双位数都清）。

## ⚡ 最新状态速报（2026-10-28 晚·八，R5 稳定性收口——WER 崩溃转储落地）

- **WER LocalDumps 本机落地**：tools\setup_dumps.cmd（脚本早备好从未执行）提权跑通并实证 HKLM——`LocalDumps\LingxiUI.exe` DumpFolder=%TEMP%\lingxi-dumps / DumpType=2 全量 / DumpCount=5，**此后 LingxiUI 崩溃自动落全量 dump**。DLL 侧不装 VEH/UEF 铁律不变。
- **SignalNamedEvent 静默失败 P2 已修**（ui/main.cpp）：二实例转发 --symbols/--settings 时主实例 4s 未就绪不再静默丢命令，LX_LOG + 弹一次提示；regress ALL PASS，真实 UI 已重启（PID 20772）。
- **PITFALLS #34 新增**：PowerShell 嵌套 powershell 外层双引号先插值 `$env:`——内层赋值形同虚设，嵌套命令体一律单引号或同层执行。
- R5 剩余：32 位 TIP；R3 LLM 整句重排方案就绪未实施（cur/GLM_EXPLORATION_R3_LLM.md）。Loomy 整改 B1-B8 全收官（含 B2 像素取证补遗）。

## ⚡ 最新状态速报（2026-10-28 晚·七，B2 窗体像素取证补遗——验证实证收官）

- **B2 回显渲染实证完成**：假客户端探针（MSG_CTX pinyin="nihao"）驱动真实候选窗出窗 + 特征色像素分析——composing 底色带 RGB(240,244,250) 窗顶 42 行横带、其下候选区、accent 蓝字跨回显带；en 对照（flags=0）无回显带且候选区上移 42 行=因果实锤（回显行只在有组合串时出现且候选区同步下推）。B2 落地时"窗体无自动化断言人眼验收"的边界补齐为像素实证。探针产物已清理，数据固化 cur/LOOMY-HANDOFF.md §B2 补遗、DEVLOG 晚（七）小节。**B1/B8/B4/B3/B5(①②)/B2 全部完成，Loomy 整改全链收官。**

## ⚡ 最新状态速报（2026-10-28 晚，B5 草案② toggleMode 设置升级——Loomy 整改全链终章）

- **toggleMode 完成（B5 草案②，收官后补遗）**：`shiftToggle(bool)` → `toggleMode(0 无/1 Shift/2 Shift 或 Ctrl)`——settings 双读兼容（新键 ToggleMode 优先/旧键 ShiftToggle 派生，保存双写旧版无感）；DLL 切换键状态机扩展 Ctrl（mode 2）+ Shift/Ctrl 交错保护（Ctrl+Shift+K 类快捷键不误切）；设置页复选框→三态下拉。**用户零感知**：现值派生 toggleMode=1 原行为不变。regress ALL PASS（T16 派生路径全过=双读端到端实证）。真实 UI 已重启（PID 26956）。至此 LOOMY §B3-B5 草案①②全落地（③全局热键=R4 尾巴未动）。
- **B2 完成（backlog 收官）**：非 inline 模式候选窗首行回显拼音组合串——`CandWindow::Show` 加 composing 参数（纯 UI 内部不走协议，DLL 零改动），PredictProc 传 `e.pinyin ? e.text : L""`，重排线程同 ver 穿透不跳变；高度 +1 行、回显行宽度参与竞争、hover/点击行判定同步偏移（点回显行不选词）。GDI 无 alpha 用一档实色近似半透明区分。窗体绘制无自动化断言（人眼验收）。**B1/B8/B4/B3/B5/B2 全部完成**，Loomy 体验整改收官。
- **B5（草案①）亦完成**：deep_test T16 四断言（单击切临时英文/再单击切回/Shift 组合使用不误切/组合中单击不丢组合），TapKey/RawKeyUp 新辅助；**顺带修真 bug**：组合中 `KeyDown(VK_SHIFT)` 落 HandlePinyinKey unmapped-default 误提交当前首选——分发层单列放行修复（weasel preserved key 等价语义），T16d 即其回归断言。flaky 归因方法论（失败首发位置+单因一致性）固化 PITFALLS #33。

## ⚡ 最新状态速报（2026-10-28 晚，B2 候选窗拼音回显——Loomy backlog 收官）

- **B2 完成（backlog 收官）**：非 inline 模式候选窗首行回显拼音组合串——`CandWindow::Show` 加 composing 参数（纯 UI 内部不走协议，DLL 零改动），PredictProc 传 `e.pinyin ? e.text : L""`，重排线程同 ver 穿透不跳变；高度 +1 行、回显行宽度参与竞争、hover/点击行判定同步偏移（点回显行不选词）。GDI 无 alpha 用一档实色近似半透明区分。窗体绘制无自动化断言（人眼验收），regress 功能面全 PASS（latency 红=已知漂移）。真实 UI 已重启（PID 29372）。**B1/B8/B4/B3/B5/B2 全部完成**，Loomy 体验整改收官。
- **B5（草案①）亦完成**：deep_test T16 四断言（单击切临时英文/再单击切回/Shift 组合使用不误切/组合中单击不丢组合），TapKey/RawKeyUp 新辅助；**顺带修真 bug**：组合中 `KeyDown(VK_SHIFT)` 落 HandlePinyinKey unmapped-default 误提交当前首选——分发层单列放行修复（weasel preserved key 等价语义），T16d 即其回归断言。flaky 归因方法论（失败首发位置+单因一致性）固化 PITFALLS #33。草案②（toggleMode 设置页升级）待用户拍板、③（全局热键）=R4 尾巴未动。
- 潜在后续（backlog 外）：LOOMY §B5 草案② toggleMode 设置页升级；§B6 词库质量类通道（fix_readings/modern_terms）常备。

## ⚡ 最新状态速报（2026-10-28 晚，B5 Shift 切换断言 + 组合中 VK_SHIFT 误提交修复）

- **B5（草案①）完成**：deep_test T16 四断言（单击切临时英文/再单击切回/Shift 组合使用不误切/组合中单击不丢组合），TapKey/RawKeyUp 新辅助；LX_LOG 取证 `Shift toggle` 翻转对。**顺带修真 bug**：组合中 `KeyDown(VK_SHIFT)` 落 HandlePinyinKey unmapped-default 误提交当前首选——分发层单列放行修复（weasel preserved key 等价语义），T16d 即其回归断言。flaky 归因方法论（失败首发位置+单因一致性）固化 PITFALLS #33。草案②（toggleMode 设置页升级）待用户拍板、③（全局热键）=R4 尾巴未动。真实 UI 已重启（PID 18700）。
- backlog 剩余：**B2 候选窗拼音回显**（B1/B8/B4/B3/B5 已完成，见下方各 ✅ 行与 cur/LOOMY-HANDOFF.md）。

> 最近更新：2026-10-28 凌晨（QC 代理代更：R2-R4 功能面全部落地、Edge 兼容用户实测确认、R3/R5 预探索完成）。**任何代理读完本文档即可直接接手。**
> 配套阅读：[PROGRESS.md](PROGRESS.md) · [DEVLOG.md](DEVLOG.md) · [PITFALLS.md](PITFALLS.md)（20 条踩坑，#17-#19 为崩溃三段式）· [README.md](../README.md) · 双代理协作目录 [cur/](../cur/)（含 QC 报告与 R2-R5 预探索指南）

## ⚡ 最新状态速报（2026-10-28 晚，B1 部分提交全链路落地）

- **B3 完成**：零声母 i/u 键死根治——词典无 i/u 打头规范写法（修前 probe 8 用例全 0 候选实证）。引擎 `ZeroInitialShorthand()` 全 20 项表（i→yi…uo→wo）在 `EnumeratePaths` DP 内扩展（cost=2 与全拼/简拼竞争），`SyllUnit.rawLen` 保 cover 回放正确（i→yi cover=1 非 2）。engine_test P6d +8 断言、deep_test T15 10 Report（i→一/u→无/in→因/iu→有、退格缩减、vMode 两路退出、标点合并）。v 首键=vMode 直通英文（设计如此），DLL 侧零改动。
- **同轮根治测试基建楔死（PITFALLS #32，重要）**：deep_test/e2e 程序化激活从旧 `ActivateLanguageProfile` 换 **`ITfInputProcessorProfileMgr::ActivateProfile`**（Win8+ 完整切换路径）+ settle 窗口 3s→15s。根因：旧 API 激活后 msctf 按键派发有可变长就绪暗窗（1s～60s+），"注册 ✓+active ✓+零派发"是本坑签名；修后 deep_test 6 连 22/22、regress ALL PASS。今后任何程序化激活 TSF TIP 的测试代码都走 ProfileMgr。
- **B1 完成**（引擎 `b7d0ee1` + 管道/DLL `f84f049`）：候选携带 cover（消耗的拼音前缀字符数），选短词后剩余拼音留组合继续；学习键改消耗前缀根治错位学习。协议向后兼容（MsgCands 可选尾部，kProtoVer 不变）。regress ALL PASS（engine_test 147 / fix_readings / deep_test T14 端到端 / e2e）。
- **注意**：候选序 probe 别信历史记录——模糊音默认开，"你"在 nihao 序 idx7（键'8'，engine_test P6c4 pin）；deep_test 日志中文乱码，取证用码点 hex 转储（PITFALLS #29/#30）。
- backlog 剩余：B5 Shift 断言 → B2 候选窗拼音回显（B8/B4/B3 已完成，见下方各 ✅ 行与 cur/LOOMY-HANDOFF.md）。
- **✅ B8 亦完成（`06548a8`）**：symbolHotkey 默认关 + 设置页文案（Edge 冲突注明）；用户 HKCU 残留已清 0，真实 UI 已重启（热键即时释放）。
- **✅ B4 亦完成（`2b3b477`）**：纯英文上下文（整段<0x80）链路由改词表优先、LLM 兜底候选过滤为纯 ASCII（全滤空则无候选）；复读裁剪升级 StripContinuationEcho（标点归一/tail 全长上限/整段复读循环剥）。ChainPredictor 与裁剪移入 ngram.cpp（engine_test 不编 llm.cpp），+14 断言全 PASS；`--probe-chain` 真机端到端："Today is a nice "→纯 ASCII 英文续写、inte→词表 0ms 短路、中文上下文不变。probe 盲区注意：`--probe-llm` 直连 LlmPredictor **不经链路由**，B4 类路由/过滤只能 `--probe-chain` 验证。真实 UI 已重启（PID 7616）。剩余 backlog：B3 → B5 → B2。

## ⚡ 状态速报（2026-10-28 下午轮，历史基线——**其后的晚·十三/十四更新见下方另一速报，为准**）

- 13 提交后工作区干净，regress 三件套+词典闸门全绿；R3 重排从 WIP 到全部落地并按实测**默认关**（打字 CPU 66%->3.5%，候选窗抖动清零）。
- 词库进入管线时代：tools/fix_readings.py（多音补全+--check 闸门，228 字/239 读音）+ tools/modern_terms.txt（现代词层，build_pinyin --modern-only 幂等）。
- 体验三修：提交键候选有界等待（快打不丢汉字）、切分分池（长串候选 27->60）、模式检测竞态自愈；deep_test 新增 T12/T13，engine_test 142 断言。
- **当前生效状态**：LingxiUI.exe 已真实环境重启（新词库/新 LLM 链在跑）；用户应用里的 DLL 侧修复需重启应用。最新交接=**cur/LOOMY-HANDOFF.md**（含 B1-B8 未完成项与取证设计）。

（下方凌晨速报为历史背景。）

## ⚡ 最新状态速报（2026-10-28 凌晨：R2-R4 体验面全落地 + Edge 结案 + R3/R5 方案就绪）

- **git**：HEAD `b6c4aa5`（以下提交均在 main）；工作区常有开发代理在制品，以 `git status` 为准
- **QC 修复已合入 main（39535f5 / 2490ed5 / db50e36）**：`_userDict` 三线程数据竞争（引擎 `_udMtx` 粗锁 + 用户词典两侧原子落盘）、DllUnregisterServer 失败向上传播（杜绝幽灵 TIP 假成功）、PROGRESS/PITFALLS 对齐。验证门：构建 ✅ / engine_test 119 PASS ✅ / e2e exit=0 ✅
- **Edge 兼容正式结案（用户实测确认）**：Edge 无法输入问题已解决——无组合模式（`_inlineComp=false`）+ `AbortCompositionState` 兜底 + ReadFrame 持久缓冲等隔离性修复在真实 Edge 宿主验证通过（详见 cur/USER-CONFIRM-EDGE.md）
- **符号面板全链路上线**：5 分类符号/emoji 网格（码点级代理对处理）、点击经 IPC 插入宿主、托盘入口、Ctrl+Shift+B 全局热键（SymbolHotkey 设置可开关，动态注册/注销）、WS_EX_NOACTIVATE 不抢宿主焦点
- **单实例命令转发**：命名事件 + 隐藏通知窗口 + 等待线程；第二个实例（--symbols/--settings）转发命令给已运行实例后退出——宿主 TIP 自动拉起的无参数实例不再抢占单实例
- **小鹤双拼**：`ShuangpinToQuanpin` 在 Predict 入口转换后复用全拼链路（声母 v/i/u、韵母歧义键按声母消解、零声母补齐、ü 三态）；engine_test 端到端用例（jbtm→今天）
- **繁体输出**：OpenCC STCharacters 字符映射（4012 字），码点级代理对处理；Traditional 设置开关
- **用户词升级**：新词/自定义词入主索引 + 首字母串简拼索引，Learn 后即时重建，缩写（mm）可调出已学新词
- **候选窗体验**：hover 非首行浅色高亮；字体缓存复用（不再每次绘制建字体）
- **ReadFrame 丢帧修复（重要）**：原实现单次 ReadFile 读入多帧时丢弃余量字节，连发帧被静默吞掉——改为调用方持久 rxbuf + 消费后 erase；两处调用方（DLL pipeclient / UI pipeserver）均已适配
- **R3/R5 预探索完成（替开发代理省探索时间，照做即可）**：LLM 整句重排/多路采样/KV 复用全方案见 cur/GLM_EXPLORATION_R3_LLM.md（API 对 llama.h 行号核实、GGUF 头实测；前置堵点：拼音模式 DLL 必须先补发上文）；R2 缺口/R4 插入点/R5 工程化见 cur/GLM_EXPLORATION_R2R5.md
- **双代理协作机制**：cur/ 为沟通目录（QC 报告/QC-REPORT.md、审查意见、用户确认、探索指南均在）；每 20 分钟 QC 自动巡视；铁律见 cur/HANDOFF.md

## ⚡ 前情速报（M5 阻塞已解除：启动崩溃根因修复 + 框架异常遏制）

- **崩溃已修复并验证**：e2e 默认门控 exit=0；`LINGXI_PINYIN=1` 也 exit=0（tests 1-5 全 PASS，拼音组合用例为诊断级）。engine_test 9/9 PASS（含拼音 4 项，延迟 0.32ms）。进程零崩溃（框架级 AV 全部被 SEH 遏制）
- **根因（三段式，详见 PITFALLS #17-#19）**：① 激活 sink 挂到 ITfThreadMgr 的 ITfSource（错误对象）→ 写坏 CTF 内部 sink 表 → 之后任意堆分配 AV（崩在 Deactivate→Send 只是接盘）；② 修掉后按键分发内调 GetActiveLanguageProfile 又触发输入上下文栈重算（候选窗锚点全零）；③ 语言切换 churn 中 TSF 框架组合操作自身脆弱（textinputframework 读 -1 / 陈旧 store sink DEP）。产品侧已全部防御：不用激活 sink（改键路径语言变化+置脏惰性检测）、组合键处理全程 SEH 包裹 + 异常安全复位降级直通
- **git**：修复与加固已提交至 `6181df0`（M5 崩溃修复 / e2e 加固 / 构建锁文件修复三个提交）
- **M4 直通+预测路径不受影响**：默认门控下 e2e 全绿
- **真实应用反馈修复（commit `75bf6ee`）**：用户实测所有应用里 zh-CN 只有英文候选——根因是纯 TSF TIP 切 profile 时线程 HKL 不变，旧判据 `GetKeyboardLayout` 判语言永远为假（e2e 的 LoadKeyboardLayout 伪造掩盖了）。已改 `GetActiveLanguageProfile` 真值 + 封送窗口消息泵空闲点检测（修正 PITFALLS #18 过宽结论）；另：候选 5→9、GetTextExt 空矩形兑底。e2e 全绿（tests 1-5 直通判定正确、test 6 拼音组合进入）
- **托盘/设置已上线（commit `9af1bea` + `a56d325`）**：托盘左键设置窗、右键菜单（拼音模式 ✓ / 暂停预测 ✓ / 设置 / 退出）。设置窗口：拼音模式、英文逐键补全、候选数量（1-9）、明暗（跟随应用/浅/深）。存 HKCU\Software\LingxiIme；DLL（拼音开关）与 UI（其余项）实时读取，改动即时生效（按键路径单值探测，托盘切换后当前窗口下一键生效）。新建：common/settings.*、ui/settingsui.*、ui/app.ico|rc。冒烟：CandidateCount=2 → 全链路候选数=2
- **拼音体验补充（commit `824ee56`）**：Shift/CapsLock 中英混排直通（组合态先提交字母原文再放行大写字母）；标点触发汉字首选上屏（无候选才保留字母）
- **新旧 DLL 判别**：候选窗右下角构建时间戳，看不到 = 旧版 DLL（需重启该应用）
- **e2e 拼音用例调整**：合成宿主内实际打拼音组合会诱发 TSF 框架（textinputframework）内部状态损坏，AV 从框架自身消息泵路径逃逸、无法遏制 → 用例改为"拼音模式判定"断言（确定性通过），组合转换逻辑由 engine_test 9/9 背书，真实表现靠用户实测
- **拼音分词升级（commit `54f24d3`）**：贪心词链改为词频加权 Viterbi（Σlog(1+名次) 全局择优）+ 末字变体候选。实测：`jintianqi` → 今天其/今天起/今天气/今天七/进天气/今天/今天起来/今天企业（8 候选，末字变体全可见）；`nihao` → 你好/你号/你灏/你豪/泥好/尼好/你。engine_test 9/9、e2e 绿。待做：LLM 重排（上下文语义消歧）、前缀提交语义（空格提交词首、剩余音节继续组合）
- **遗留待办**：合成宿主内拼音组合在语言 churn 后无法完整走通（TSF 框架过渡态，已遏制不崩宿主）；若需彻底验证可重启电脑后空闲时跑 e2e

## 一、项目一句话

Windows 端 TSF 系统级 AI 输入法（代号灵犀）：两进程架构（ImeCore.dll 注入宿主 + LingxiUI.exe 候选窗/引擎）。**双模式**：en-US profile = 英文直通+补全/续写（M4 LLM 已实测工作）；zh-CN profile = 拼音组合输入（M5，代码完成待崩溃排查）。

## 二、当前状态（git 已提交至 `6181df0`，全部可构建、可回归）

| 组件 | 状态 |
|---|---|
| ImeCore.dll（TSF 服务） | ✅ 稳定：注册/激活/直通/上下文读取/插入+前缀替换上屏/跨线程封送；双模式（en-US 直通+预测 / zh-CN 拼音组合，门控中） |
| LingxiUI.exe（候选窗+引擎） | ✅ 稳定：候选窗、异步预测管线（队列+120ms 防抖+alive 检查）、拼音引擎接入 |
| 预测引擎 | ✅ ChainPredictor：**词中→词表补全**（mor→more，inte→international）；**词边界/中文→LLM 优先**，超时/缺失→词表兜底 |
| 拼音引擎（pinyin.cpp） | ✅ engine_test 9/9：`jintianqi→今天…`、`ni hao→你好`、部分音节尾、非法串空、延迟 0.32ms |
| LLM 引擎（llm.cpp） | ✅ 已验证工作：动态绑定 llama.dll（b11900），Qwen2.5-0.5B-Instruct Q4（491MB），decode+logits 贪心，24 token/300ms 预算 |
| 测试 | ✅ e2e（默认门控 exit=0；拼音开启也 exit=0，组合用例诊断级）；engine_test 9/9 PASS |
| 崩溃遏制 | ✅ 组合键处理全程 SEH（HandlePinyinKeySEH + AbortCompositionState）；e2e 有 VEH 崩溃取证（%TEMP%\lingxi-e2e-crash.txt）与 TestStore 锁 SEH |

**⚠️ 唯一未闭环项**：LingxiUI 正常启动时的 LLM 预热线程加载速度受 Defender 首次扫描影响（实测中模型加载期间 CPU/内存正常增长=在干活，未观察到最终"LLM 引擎就绪"日志行即收尾）。**这是性能问题不是功能问题**——同步探针（`LingxiUI.exe --probe-llm`）已证明全链路正确。接手后第一件事：跑 `--probe-llm` + e2e 确认，再考虑预热优化。

## 三、接手后立即要做的事

```cmd
cd /d D:\Visual Studio Code\Something\LingxiIme
tools\build.cmd                 :: 全量构建（含词表/llama 运行时/模型的 robocopy 就位）
bin\Debug\engine_test.exe       :: 引擎+拼音单测，应 9/9 PASS
bin\Debug\e2e.exe               :: 端到端回归，exit=0（机器空闲时跑更稳；拼音用例已默认含在内）
bin\Debug\LingxiUI.exe --probe-llm   :: LLM 诊断（看 %TEMP%\lingxi-ime.log 的 [llm] 行）
bin\Debug\LingxiUI.exe --probe-pinyin :: 拼音候选诊断（[pinyin] 行，常见输入首选/候选列表）
```

- 注册状态：已注册（HKLM，管理员完成）。`tools\imecheck.ps1` 可验证。**重建 DLL 不需要重新注册，但已运行的应用会继续用旧版 DLL**（CTF 加载后改名不换新）——验证新行为前先重启目标应用或注销重登
- 日志：设 `LINGXI_LOG=1` → `%TEMP%\lingxi-ime.log`（**UTF-16LE**）。注意：e2e 每 attempt 删日志文件，DLL 缓存句柄会写已删除文件（代际混杂，取证用 VEH 崩溃文件 + stdout）
- 测试注意：用户正在用机器时 ctfmon 会反复抢前台（TIP 去激活/重激活 churn），e2e 会受干扰——重试壳 + 就绪等待已缓解，理想是空闲时跑；偶发首断言 fail-fast 属预期
- **git 规矩（用户要求）**：小步提交、每步验证、提交信息中文。仓库在 LingxiIme/ 内，`core.autocrlf=false`，`* -text`

## 四、下一步开发路线（按优先级）

1. **M5 手工验证 + 移除门控**：Win+Space 切 zh-CN → 打 `jintian` → 空格出"今天"、数字选词、Esc 取消、Enter 保留字母；通过后把 composition.cpp DetectPinyinMode 的 env 门控与 e2e 测试 6 门控移除，默认启用并全量回归
2. **M5 收尾**：候选排序接入 LLM 重排（当前词频）；组合串编辑（光标移动/选中删除）；标点顶末音节提交
3. **M4 收尾**：LLM 候选质量调优（prompt/温度/top-k 多路采样出 3-5 个候选）
4. **M4.5 KV cache 复用**：llm.cpp 里 TODO——上下文增量追加/退格回退（`llama_kv_self_seq_rm` 类 API），把稳态预测压到 10-30ms
5. **M6 打磨**：设置界面、隐身热键（Esc 三连/托盘开关）、卸载清理、签名、32 位 TIP
6. **工程**：vcpkg 或脚本化 llama.cpp 升级流程；DLL 日志句柄失效重开（见 PITFALLS #20）

## 五、环境速查（本机实况）

- 工具链：VS2022 Community（v143）+ WinSDK 10.0.26100；**无 CMake/Ninja**（llama.cpp 用预编译二进制，不要尝试本地构建）
- llama 资源：`deps/llama/bin`（30 个 DLL，b11900，tag 形如 bXXXX 不是 v0.x——`releases/latest` 返回的 v0.7.0 无二进制）、`deps/models/qwen2.5-0.5b-instruct-q4_k_m.gguf`；ggml 头文件在 `deps/llama/include`（注意新版仓库 ggml 头在 `ggml/include/` 子目录）
- 构建：`tools/build.cmd [Debug|Release]`；**构建被锁不用慌**（build.cmd 自动改名旧产物，被 CTF 加载的 DLL 能改名不能覆盖）
- 常驻进程：用户机器上有 ZCode 里的旧 DLL 实例，会自动重启 LingxiUI（无日志环境）——测试前 `taskkill /IM LingxiUI.exe /F`，日志缺失别慌，是环境实例没继承 LINGXI_LOG
- 中文输出：控制台 GBK 乱码属正常；.cmd/.ps1 必须 ASCII

## 六、验证基线（本次收尾实测）

- e2e：默认门控 exit=0（attempt 3 全 PASS，重试壳吸收环境噪声）；`LINGXI_PINYIN=1` 也 exit=0（拼音组合用例诊断级 WARN：字母可进组合但合成宿主语言 churn 后被遏制降级——见 PITFALLS #19）
- engine_test：9/9 PASS（拼音 4 项 + 延迟断言，`jintianqi` 平均 0.32ms）
- 崩溃遏制：一次框架级 AV（DEP/陈旧 sink）被 SEH 遏制，进程零死亡；VEH 取证文件正常记录
- 注册自检：PASSED（CLSID + TIP + 双 profile）
