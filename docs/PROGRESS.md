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
| M5 拼音输入 | 🔶 代码完成，启动崩溃已修复 | 组合窗口 + 拼音引擎（engine_test 9/9，延迟 0.32ms）+ 双 profile 双模式；`LINGXI_PINYIN=1` 启用。合成宿主内组合受语言 churn 限制（已 SEH 遏制，进程零崩溃），真实宿主待手工验证；崩溃根因与对策见 [PITFALLS.md](PITFALLS.md) #17-#19 |
| M6 打磨 | ⬜ 未开始 | 设置界面、隐身热键、卸载清理、代码签名、32 位 TIP |

## 已验证的能力（e2e 自动化证明）

- 按键直通：激活后正常打字零干扰
- 候选窗：跟随光标真实显示、深浅色自适应、点击不抢焦点
- 英文前缀替换：`mor` → `more`，`inte` → `international`（Tab 上屏）
- 上屏后继续直通、多应用多实例管道、焦点切换后预测不失效
- 中文上下文续写候选（词频级质量）
- 密码框架构性隔离（TSF 框架层保证）

## 已知限制

- 激活灵犀时 zh-CN profile 下字母进入拼音组合（`LINGXI_PINYIN=1` 时）；未开启时 zh-CN 下无法打汉字（拼音转换是 M5，代码已就绪待启用）
- 拼音组合在合成测试宿主内受语言切换 churn 限制（TSF 框架过渡态，已 SEH 遏制）；真实应用表现待手工验证
- 中文续写质量为词频级占位（M4 LLM 接管）
- 仅 x64 宿主；未签名（SmartScreen/杀软可能告警）
- e2e 在"用户正在用机器"时会受 ctfmon 前台竞态干扰（重试机制 + 就绪等待已缓解）

## 工程资产

- `tools/build.cmd`：锁文件预改名（被 CTF 加载的 DLL 能改名不能覆盖）
- `tools/register.cmd` / `unregister.cmd` / `imecheck.ps1`：注册与自检
- `tools/e2e/e2e.cpp`：端到端测试宿主（合成 ITextStoreACP + 模拟按键）
- `tools/build_dict.py`：词表加工（g10k + jieba → en/zh 词表）
- `deps/win-classic-samples`：官方 TSF 样本参照（step01-06 + tsfapp）
