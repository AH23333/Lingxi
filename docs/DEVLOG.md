# 灵犀输入法 — 开发日志（DEVLOG）

> 按时间顺序记录每个里程碑做了什么、为什么这么做。踩坑细节见 [PITFALLS.md](PITFALLS.md)，当前状态见 [PROGRESS.md](PROGRESS.md)。

## 2026-10-26 · M0 脚手架 + M1 TSF 骨架

**决策：两进程架构。** TSF DLL 被加载进所有宿主应用进程，DLL 崩溃 = 宿主崩溃，所以模型推理和候选窗 UI 必须隔离到独立进程（LingxiUI.exe），DLL 只做按键转发/上下文读取/上屏。微软拼音同架构。

- 生成 CLSID/双语言 profile GUID（en-US 主配置 + zh-CN 便于中文测试）
- 稀疏克隆微软官方 TSF 样本（`Windows-classic-samples/.../tsf/textservice/`，含 step01-06 分步教程）到 deps/ 作对照
- **注册攻坚**：先尝试 per-user（HKCU）——CLSID 可以写进去，但 CTF 的 `ITfInputProcessorProfiles::Register` 非管理员一律 E_FAIL，只能提权注册；第一次提权后 Register 仍失败，根因是 **InprocServer32 路径含空格必须加引号**（"D:\Visual Studio Code\..."）。修复后注册成功。
- 编译脚本被 GBK 坑了一次：cmd.exe 按 ANSI 读 .cmd，UTF-8 中文注释直接乱码，批处理一律纯 ASCII。

## 2026-10-26 · M2 候选窗 + 上屏

- LingxiUI.exe：置顶无边框候选窗，`WS_EX_NOACTIVATE` 保证点击不抢宿主焦点，深浅色自适应，DPI 感知
- 上屏实现两种：`ITfInsertAtSelection`（纯插入）+ 范围 `SetText`（英文前缀替换，协议加 `replaceLen` 字段——否则 "mor"+Tab 会变成 "mormorning"）
- 鼠标点选候选发生在 UI 进程，跨线程封送：pipe → DLL 的 IO 线程 → PostMessage 到 DLL 内隐藏窗口 → 宿主 UI 线程执行编辑会话

## 2026-10-27 · e2e 自动化测试（自我测试的转折点）

用户要求自己测。写了 TSF 标准做法的测试宿主：合成 `ITextStoreACP` 文本存储 + `AssociateFocus` 绑定真实窗口 + `ITfKeystrokeMgr` 模拟按键 + 直接断言缓冲区。

**自测揪出 4 个真实产品 bug**（详见 PITFALLS.md 前四条）：

1. `AdviseKeyEventSink` 的 `fForeground` 必须传 TRUE——否则按键永远不路由给我们
2. `MSG_ACTIVATE` 在管道连接前发送被丢弃 → UI 永远不知道谁是活动客户端 → 候选窗永不显示。修复：连接建立后补发快照
3. 管道写入没串行化：HELLO/ACTIVATE（管道线程）与 SendCtx（宿主 UI 线程）并发写帧交错损坏。修复：整帧写入持锁
4. **`ITfThreadMgrEventSink::OnSetFocus` 参数顺序搞反**（SDK 语义：第一参数=获得焦点的文档）。挂错方向后，每次焦点切换编辑 sink 就停在旧文档上，预测从此失灵——**这正是用户在真实应用里"切几次窗口就没有候选窗"的根源**

另确认两个环境事实：候选窗应跟随"最后打字的应用"（多客户端时 ACTIVATE 时序不如 CTX 时序可靠）；e2e 会与 ctfmon 抢前台（用户在用机器时测试有干扰，测前需要重试机制）。

## 2026-10-27 · M3 真词表引擎

用户反馈"只出英文且无候选窗"——分析为两个叠加原因：(1) v1 直通形态本来打不出汉字（拼音在 M5）；(2) 占位引擎只有 36 个词。

- 数据：google-10000-english（英文 1 万词，词频序）+ jieba 词库（MIT）取 top 80k 中文词（过滤含 ASCII/纯符号），`tools/build_dict.py` 加工
- 引擎：线性前缀匹配（1 万词 <0.1ms），英文保留用户大小写；中文输出高频接续词（诚实定位：无共现语料，质量占位，M4 LLM 接管）
- 健壮性：transitory 上下文（CUAS 宿主）只支持同步编辑会话 → 所有 RequestEditSession 改为"同步优先、TS_E_SYNCHRONOUS 降级异步"
- e2e 全绿（15 项断言 PASS，exit=0）

## 2026-10-27 · M4 本地 LLM 引擎（进行中）

用户要求"多用子代理全力开发"。三线并行：资源线（llama.cpp 预编译 + Qwen 0.6B GGUF，ModelScope 源）/ 引擎线（LoadLibrary 动态绑定，不参与链接，`__has_include` 探测头文件——deps 未就绪时自动编译为空实现）/ 测试线（engine_test 单元测试 + e2e 抗干扰重试）。

设计要点：
- 生成走最稳路径：`llama_decode` + `llama_get_logits_ith` 自实现贪心 argmax（采样器 API 跨版本变动大，logits 路径十年稳定）
- 结构体参数（model/context params）来自**与二进制同 tag 的头文件**，避免布局漂移；函数指针全部 GetProcAddress，任一缺失 → 整体降级
- 管线异步化：CTX 入队（每客户端只留最新）→ 预测工作线程（120ms 防抖 + 300ms 预算）→ LLM 空/超时 → n-gram 兜底 → alive 检查后应用

## 待办

- [ ] M4 集成回归（资源下载完成后）
- [ ] M4.5：KV cache 增量复用（退格回退而非全量重评）
- [ ] M5：拼音输入（组合窗口 + 音节切分 + 词→拼音表 + AI 重排）
- [ ] M6：设置界面、隐身热键、卸载清理、代码签名
