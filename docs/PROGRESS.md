# 进度看板（PROGRESS）

> 一页看清：完成了什么、正在做什么、下一步。详细过程见 [DEVLOG.md](DEVLOG.md)，坑见 [PITFALLS.md](PITFALLS.md)。

## 里程碑

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M0 脚手架 | ✅ 完成 | 目录/构建（MSBuild 零额外安装）/GUID/日志/IPC 协议 |
| M1 TSF 骨架 | ✅ 完成 | 注册（双语言 profile）、激活、按键直通、全链路日志 |
| M2 候选窗+上屏 | ✅ 完成 | 跟随光标、鼠标/键盘选词、插入与前缀替换两种上屏 |
| e2e 自动化测试 | ✅ 完成 | 合成 ITextStoreACP 宿主 + 模拟按键；3 次重试壳（ctfmon 前台竞态为环境特性）|
| deep_test 深度回归 | ✅ 完成（2026-10-28 下午） | 拼音组合全管道场景 T0-T13（含用户场景固化：翻页+第二页选词、60ms 连打 30ms 抢空格）；settle 失败可取证 |
| M3 真词表引擎 | ✅ 完成 | 英文 g10k 前缀补全 + 中文 jieba 高频接续词（8 万词条） |
| M4 本地 LLM 引擎 | ✅ 完成 | llama.cpp b11900 动态加载 + Qwen2.5-0.5B Q4 + 上下文感知降级链 + 异步预测管线；探针实证工作正常。详见 [HANDOFF.md](HANDOFF.md) |
| M4.5 KV 增量复用 | ✅ 已落地（R3 轮） | 双上下文物理隔离（续写/重排互不驱逐，实测 141→141ms）+ prefill abort 硬闸 + EMA 吞吐自动降级 + SEH 熔断 |
| M5 拼音输入 | ✅ 默认启用 | 组合窗口 + 拼音引擎（engine_test 142 项断言）+ 双 profile；真实应用激活判据已修（PITFALLS #17/#18）；候选窗即时候选（免防抖，每键 RTT ≈57ms）+ 提交键候选有界等待（快打不丢汉字）；拼音模式激活竞态已自愈 |
| M6 打磨 | 🔶 设置+符号面板已上线 | 托盘图标 + 设置窗口（拼音模式/英文补全/候选数量/明暗/中文标点/模糊音/Shift 切换/v 模式/双拼/繁体/**LLM 重排开关**/符号热键开关 + 用户词典管理，HKCU 存储即时生效）；符号面板（5 分类/emoji 代理对）；单实例命令转发。待做：字号/皮肤细节、卸载清理、代码签名、32 位 TIP |
| R1-R4 拼音体验 | ✅ 完成 | 词库 30 万 + 口语层；Viterbi 整句 + 末字变体；四层排序；模糊音；中文标点；翻页（PageUp/Down 与 -/=）；双拼；繁体；自学习；hover 高亮 |
| R3 LLM 整句重排 | ✅ 已落地（默认关） | 指令式重排 top-8 + 双上下文 KV + abort/EMA/SEH + rrSeq 连击合并；实测把打字 CPU 从 66% 降到 3.5% 后按"不默认强加"原则关为默认，设置页可开；多路采样未做（cur/GLM_EXPLORATION_R3_LLM.md §3）|
| 词库数据管线 | ✅ 完成（2026-10-28 下午） | fix_readings 多音补全（228 字/239 读音）+ modern_terms 现代词层（176 词，只升不降 band 置顶）；`--check` 已挂 regress 闸门；全管线幂等可重放（PITFALLS #25/#26）|
| R5 工程化 | 🔶 预探索完成部分落地 | regress.cmd 一键回归已上线（engine+词库闸+deep+e2e，环境沙盒）；32 位 TIP/卸载清理/签名未做（方案见 cur/GLM_EXPLORATION_R2R5.md）|

## 已验证的能力（自动化证明）

- 按键直通：激活后正常打字零干扰
- 候选窗：跟随光标真实显示、深浅色自适应、点击不抢焦点
- 英文前缀替换：`mor`(→more 替换上屏)、`shi`=是(上下文感知)
- 上屏后继续直通、多应用多实例管道、焦点切换后预测不失效
- 中文上下文续写候选（词频级质量）
- 密码框架构性隔离（TSF 框架层保证）
- 快打抢空格上屏汉字不丢候选（e2e pinyin SKIP 项已修转活，见 DEVLOG 10-28 下午）
- 长句（13+ 音节）+ 末尾残段候选存活、切分分池不塌缩（engine 断言坐实）

## 已知限制

- zh-CN profile 下字母进入拼音组合（默认；设置里"拼音组合模式"关闭后退回纯直通）
- 候选上限 60（分页 9/页）；语义级消歧已上线为可选 LLM 重排（默认关），**部分选词保留余段、拼音回显条、i/u/v 首字母、英文链语言闸、续写复读过滤升级、符号热键默认关**六项为当前已知体验缺口——全部有取证与设计，见 cur/LOOMY-HANDOFF.md §3
- 自绘光标应用（部分 Electron 编辑器）候选窗定位可能不准（已加系统插入符兜底）
- emoji 经 GDI 灰度渲染（彩色需 DirectWrite）
- 仅 x64 宿主；未签名（SmartScreen/杀软可能告警）
- e2e/deep 在"用户正在用机器"时会受 ctfmon 前台竞态干扰（重试机制 + 就绪等待已缓解）
- 重建后已运行的应用继续用旧版 DLL，需重启应用才吃到新键盘行为；LingxiUI.exe 常驻，换数据/换引擎需重启该进程（PITFALLS #27 三级生效）

## 工程资产

- `tools/build.cmd`：锁文件预改名（被 CTF 加载的 DLL 能改名不能覆盖）
- `tools/register.cmd` / `unregister.cmd` / `imecheck.ps1`：注册与自检
- `tools/e2e/e2e.cpp`：端到端测试宿主（合成 ITextStoreACP + 模拟按键）
- `tools/deep_test/deep_test.cpp`：拼音组合全管道深度回归（T0-T13 + 重试壳 + settle 取证）
- `tools/engine_test/engine_test.cpp`：纯进程内引擎断言 142 项（自带词库沙盒，不污染真实用户词典）
- `tools/regress.cmd`：一键全量回归（engine + 词库 `--check` 闸 + deep + e2e；环境沙盒 + 自有进程重启）
- `tools/fix_readings.py` / `tools/modern_terms.txt` / `tools/build_pinyin.py --modern-only`：词库数据管线（幂等、审计、闸门）
- `tools/pf_ux.cpp`：真实键速节奏性能探针（每键 RTT/同 ver 双帧抖动/UI 进程 CPU 采样）
- `tools/build_dict.py`：词表加工（g10k + jieba → en/zh 词表）
- `deps/win-classic-samples`：官方 TSF 样本参照（step01-06 + tsfapp）
