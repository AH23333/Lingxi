# 进度看板（PROGRESS）

> 一页看清：完成了什么、正在做什么、下一步。详细过程见 [DEVLOG.md](DEVLOG.md)，坑见 [PITFALLS.md](PITFALLS.md)。

## 里程碑

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M0 脚手架 | ✅ 完成 | 目录/构建（MSBuild 零额外安装）/GUID/日志/IPC 协议 |
| M1 TSF 骨架 | ✅ 完成 | 注册（双语言 profile）、激活、按键直通、全链路日志 |
| M2 候选窗+上屏 | ✅ 完成 | 跟随光标、鼠标/键盘选词、插入与前缀替换两种上屏 |
| e2e 自动化测试 | ✅ 完成 | 合成 ITextStoreACP 宿主，15 项断言；抗干扰重试改造中 |
| M3 真词表引擎 | ✅ 完成 | 英文 g10k 前缀补全 + 中文 jieba 高频接续词（8 万词条） |
| M4 本地 LLM 引擎 | ✅ 完成 | llama.cpp b11900 动态加载 + Qwen2.5-0.5B Q4 + 上下文感知降级链 + 异步预测管线；探针实证工作正常。详见 [HANDOFF.md](HANDOFF.md) |
| M4.5 KV cache 增量 | ⬜ 未开始 | 退格回退、前缀复用（当前全量重评估已够快） |
| M5 拼音输入 | 🔶 默认已启用，真实应用激活已修复 | 组合窗口 + 拼音引擎（engine_test 9/9，延迟 0.32ms）+ 双 profile 双模式。真实应用拼音不激活根因已修（HKL 判据无效 → GetActiveLanguageProfile 真值 + 消息泵空闲点检测，见 PITFALLS #17/#18/#18b）；候选 5→9；光标矩形空值兑底。待用户实测确认 |
| M6 打磨 | 🔶 设置+符号面板已上线 | 托盘图标 + 设置窗口（拼音模式/英文补全/候选数量/明暗/中文标点/模糊音/Shift 切换/v 模式/双拼/繁体输出/热键开关 + 用户词典管理，HKCU 存储，即时生效）；符号面板（5 分类符号/emoji，点击 IPC 插入，emoji 代理对，托盘入口 + Ctrl+Shift+B 可开关热键）；单实例命令转发。待做：字号（进行中）/皮肤、卸载清理、代码签名、32 位 TIP |
| R1-R4 拼音体验 | ✅ 完成 | 词库 8 万→30 万 + 高频口语覆盖层；Viterbi 整句分词 + 末字变体；四层排序；模糊音；中文标点；候选翻页（PageUp/PageDown 与 -/=）；v 模式直通英文；Shift 单击中英切换；自学习用户词典（选字反馈 + 管理界面导入导出 + 新词入索引 + 简拼缩写调出）；小鹤双拼；繁体输出（OpenCC 映射）；符号面板；单实例命令转发；候选窗 hover 高亮。engine_test 全 PASS |
| R3 LLM 整句重排 | 🔶 方案就绪未实施 | 全方案见 cur/GLM_EXPLORATION_R3_LLM.md（API 已对 llama.h 行号核实、GGUF 头实测；前置堵点：拼音模式 DLL 需先补发上文）；实施顺序与风险防线齐备 |
| R5 工程化 | ⬜ 预探索完成 | 32 位 TIP 可行性已核实（纯系统库依赖，WOW64 注册零代码改动）；卸载清单/WER LocalDumps/regress.cmd 方案见 cur/GLM_EXPLORATION_R2R5.md |

## 已验证的能力（e2e 自动化证明）

- 按键直通：激活后正常打字零干扰
- 候选窗：跟随光标真实显示、深浅色自适应、点击不抢焦点
- 英文前缀替换：`mor` → `more`，`inte` → `international`（Tab 上屏）
- 上屏后继续直通、多应用多实例管道、焦点切换后预测不失效
- 中文上下文续写候选（词频级质量）
- 密码框架构性隔离（TSF 框架层保证）

## 已知限制

- zh-CN profile 下字母进入拼音组合（默认；设置里"拼音组合模式"关闭后退回纯直通）
- 候选上限 9 个（数字键 1-9，可在设置中调小；翻页已上线：PageUp/PageDown 与 -/=）；长拼音串分词已用词频 Viterbi 升级（末字变体候选），语义级消歧（LLM 重排）待接
- 自绘光标应用（部分 Electron 编辑器）候选窗定位可能不准（已加系统插入符兑底）
- 设置界面已上线（托盘 + 即时生效 + 用户词典管理）；符号面板已上线（全局热键 Ctrl+Shift+B 调出，或单实例转发/启动参数）；emoji 经 GDI 灰度渲染（彩色需 DirectWrite）
- 中文续写走 ChainPredictor（LLM 优先、词表兜底），LLM 候选质量调优待做
- 仅 x64 宿主；未签名（SmartScreen/杀软可能告警）
- e2e 在"用户正在用机器"时会受 ctfmon 前台竞态干扰（重试机制 + 就绪等待已缓解）
- 重建后已运行的应用继续用旧版 DLL，需重启应用或注销重登才会换新

## 工程资产

- `tools/build.cmd`：锁文件预改名（被 CTF 加载的 DLL 能改名不能覆盖）
- `tools/register.cmd` / `unregister.cmd` / `imecheck.ps1`：注册与自检
- `tools/e2e/e2e.cpp`：端到端测试宿主（合成 ITextStoreACP + 模拟按键）
- `tools/build_dict.py`：词表加工（g10k + jieba → en/zh 词表）
- `deps/win-classic-samples`：官方 TSF 样本参照（step01-06 + tsfapp）
