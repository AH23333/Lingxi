# 交接文档（HANDOFF）

> 最近更新：2026-10-27 深夜三稿（M5 启动崩溃已修复，全链路回归绿；拼音待真实宿主手工验证）。**任何代理读完本文档即可直接接手。**
> 配套阅读：[PROGRESS.md](PROGRESS.md) · [DEVLOG.md](DEVLOG.md) · [PITFALLS.md](PITFALLS.md)（20 条踩坑，#17-#19 为本次崩溃三段式）· [README.md](../README.md)

## ⚡ 最新状态速报（M5 阻塞已解除：启动崩溃根因修复 + 框架异常遏制）

- **崩溃已修复并验证**：e2e 默认门控 exit=0；`LINGXI_PINYIN=1` 也 exit=0（tests 1-5 全 PASS，拼音组合用例为诊断级）。engine_test 9/9 PASS（含拼音 4 项，延迟 0.32ms）。进程零崩溃（框架级 AV 全部被 SEH 遏制）
- **根因（三段式，详见 PITFALLS #17-#19）**：① 激活 sink 挂到 ITfThreadMgr 的 ITfSource（错误对象）→ 写坏 CTF 内部 sink 表 → 之后任意堆分配 AV（崩在 Deactivate→Send 只是接盘）；② 修掉后按键分发内调 GetActiveLanguageProfile 又触发输入上下文栈重算（候选窗锚点全零）；③ 语言切换 churn 中 TSF 框架组合操作自身脆弱（textinputframework 读 -1 / 陈旧 store sink DEP）。产品侧已全部防御：不用激活 sink（改键路径语言变化+置脏惰性检测）、组合键处理全程 SEH 包裹 + 异常安全复位降级直通
- **git**：修复与加固已提交至 `6181df0`（M5 崩溃修复 / e2e 加固 / 构建锁文件修复三个提交）
- **M4 直通+预测路径不受影响**：默认门控下 e2e 全绿
- **门控已移除（commit `7a1dd6a`）**：zh-CN profile 默认拼音组合；`LINGXI_PINYIN=0` 可退回直通。e2e 默认即含拼音用例（诊断级），exit=0；`LINGXI_PINYIN=0` 跳过用例也 exit=0。engine_test 9/9 PASS
- **下一步第一优先**：真实宿主手工验证拼音（Win+Space 切 zh-CN → 打 `jintian` → 空格出"今天"、数字选词、Esc 取消、Enter 保留字母），反馈体验问题（候选排序/组合编辑）后进入 M5 收尾
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
bin\Debug\e2e.exe               :: 端到端回归，exit=0（机器空闲时跑更稳）
set LINGXI_PINYIN=1 && bin\Debug\e2e.exe   :: 拼音链路回归（组合用例为诊断级 WARN，不阻塞）
bin\Debug\LingxiUI.exe --probe-llm   :: LLM 诊断（看 %TEMP%\lingxi-ime.log 的 [llm] 行）
```

- 注册状态：已注册（HKLM，管理员完成）。`tools\imecheck.ps1` 可验证。**重建 DLL 不需要重新注册**（注册表指向固定路径；build.cmd 用随机后缀改名旧产物避开锁）
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
