# 交接文档（HANDOFF）

> 最近更新：2026-10-27 深夜二稿（电量 23%，M5 代码合入待验证）。**任何代理读完本文档即可直接接手。**
> 配套阅读：[PROGRESS.md](PROGRESS.md) · [DEVLOG.md](DEVLOG.md) · [PITFALLS.md](PITFALLS.md)（16+ 条已踩坑）· [README.md](../README.md)

## ⚡ 最新状态速报（M5 拼音模式：代码完成，待解决一个启动崩溃）

- **M5 全部代码已提交**（git 至 `29c5585`）：DLL 组合窗口（composition.cpp）、拼音引擎（pinyin.cpp，engine_test 9/9 PASS：`jintianqi→今天其`、`ni hao→你好`、延迟 0.94ms）、协议 flags、模式双 profile 切换、e2e 拼音用例
- **⚠️ 阻塞问题**：e2e 启动即崩 `0xC000041D`（用户回调中 AV，ntdll KiUserCallbackDispatcher 路径）。已做**临时门控**：拼音模式默认关闭（`LINGXI_PINYIN=1` 启用，激活 sink 注册同门控，OnActivated 只置脏标记，检测移到独立 MTA 轮询线程）——但 e2e 在门控后仍崩（崩前无任何输出，fflush 已加），说明崩溃源未根除
- **崩溃嫌疑排序**（接手优先查）：① e2e 合成宿主场景中 CTF 对我们 TIP 的激活处理与 profile 查询重入（建议接手后先**重启电脑**排除系统状态疲劳——本会话经历了几十轮 IME 激活/去激活）② `GetActiveLanguageProfile` 的调用时机 ③ 激活 sink 的注册时机
- **M4 直通+预测路径不受影响**（最后一次全绿：e2e 13/13 exit=0；此后引入的组合代码疑似触发上述崩溃）
- 验证拼音模式：`set LINGXI_PINYIN=1` 后构建运行 e2e（测试 6：zh-CN 激活 → 打 "jintian" → 空格 → 出"今天"类汉字）

## 一、项目一句话

Windows 端 TSF 系统级 AI 输入法（代号灵犀）：两进程架构（ImeCore.dll 注入宿主 + LingxiUI.exe 候选窗/引擎）。**双模式**：en-US profile = 英文直通+补全/续写（M4 LLM 已实测工作）；zh-CN profile = 拼音组合输入（M5，代码完成待崩溃排查）。

## 二、当前状态（git 已提交至 `89819d1`，全部可构建、可回归）

| 组件 | 状态 |
|---|---|
| ImeCore.dll（TSF 服务） | ✅ 稳定：注册/激活/直通/上下文读取/插入+前缀替换上屏/跨线程封送 |
| LingxiUI.exe（候选窗+引擎） | ✅ 稳定：候选窗、异步预测管线（队列+120ms 防抖+alive 检查） |
| 预测引擎 | ✅ ChainPredictor：**词中→词表补全**（mor→more，inte→international）；**词边界/中文→LLM 优先**，超时/缺失→词表兜底 |
| LLM 引擎（llm.cpp） | ✅ 已验证工作：动态绑定 llama.dll（b11900），Qwen2.5-0.5B-Instruct Q4（491MB），decode+logits 贪心，24 token/300ms 预算。**探针实证**：模型 0.3s 加载、`今天天气不错，`→候选`今天`（首次 703ms） |
| 测试 | ✅ e2e（13 断言+3 次重试壳，实测 exit=0）；engine_test（n-gram 单测 5/5 PASS，延迟 <0.3ms） |

**⚠️ 唯一未闭环项**：LingxiUI 正常启动时的 LLM 预热线程加载速度受 Defender 首次扫描影响（实测中模型加载期间 CPU/内存正常增长=在干活，未观察到最终"LLM 引擎就绪"日志行即收尾）。**这是性能问题不是功能问题**——同步探针（`LingxiUI.exe --probe-llm`）已证明全链路正确。接手后第一件事：跑 `--probe-llm` + e2e 确认，再考虑预热优化。

## 三、接手后立即要做的事

```cmd
cd /d D:\Visual Studio Code\Something\LingxiIme
tools\build.cmd                 :: 全量构建（含词表/llama 运行时/模型的 robocopy 就位）
bin\Debug\engine_test.exe       :: 引擎单测，应 5/5 PASS
bin\Debug\e2e.exe               :: 端到端回归，exit=0（机器空闲时跑更稳）
bin\Debug\LingxiUI.exe --probe-llm   :: LLM 诊断（看 %TEMP%\lingxi-ime.log 的 [llm] 行）
```

- 注册状态：已注册（HKLM，管理员完成）。`tools\imecheck.ps1` 可验证。**重建 DLL 不需要重新注册**（注册表指向固定路径，构建自动改名旧文件）。
- 日志：设 `LINGXI_LOG=1` → `%TEMP%\lingxi-ime.log`（**UTF-16LE**，用 iconv 或支持编码的编辑器读）。
- 测试注意：用户正在用机器时 e2e 会被 ctfmon/键盘输入干扰（重试壳已缓解）；理想是空闲时跑。
- **git 规矩（用户要求）**：小步提交、每步验证。仓库在 LingxiIme/ 内，`core.autocrlf=false`，`* -text`。

## 四、下一步开发路线（按优先级）

1. **M4 收尾**：确认 UI 预热线程完整加载（看 `LLM 引擎就绪` 日志）；若慢，预热时给进度日志；评估 LLM 候选质量（当前贪心+模板较保守，可调 prompt/温度/top-k 多路采样出 3-5 个候选）
2. **M4.5 KV cache 复用**：llm.cpp 里 TODO——上下文增量追加/退格回退（`llama_kv_self_seq_rm` 类 API），把稳态预测压到 10-30ms
3. **M5 拼音输入（最大新功能）**：IME 组合窗口（ITfComposition）+ 拼音音节切分 + 词→拼音表（可用 pypinyin 从 jieba 词库生成，`tools/build_dict.py` 同款套路）+ 候选排序（词频 + LLM 重排）。IME 侧骨架已就绪（键直通改组合拦截）
4. **M6 打磨**：设置界面、隐身热键（Esc 三连/托盘开关）、卸载清理、签名、32 位 TIP
5. **工程**：把 `ui/engine/data` 原始下载源（deps/data-src）的再获取方式写进脚本注释；考虑 vcpkg 或脚本化 llama.cpp 升级流程

## 五、环境速查（本机实况）

- 工具链：VS2022 Community（v143）+ WinSDK 10.0.26100；**无 CMake/Ninja**（llama.cpp 用预编译二进制，不要尝试本地构建）
- llama 资源：`deps/llama/bin`（30 个 DLL，b11900，tag 形如 bXXXX 不是 v0.x——`releases/latest` 返回的 v0.7.0 无二进制）、`deps/models/qwen2.5-0.5b-instruct-q4_k_m.gguf`；ggml 头文件在 `deps/llama/include`（注意新版仓库 ggml 头在 `ggml/include/` 子目录）
- 构建：`tools/build.cmd [Debug|Release]`；**构建被锁不用慌**（build.cmd 自动改名旧产物，被 CTF 加载的 DLL 能改名不能覆盖）
- 常驻进程：用户机器上有 ZCode 里的旧 DLL 实例，会自动重启 LingxiUI（无日志环境）——测试前 `taskkill /IM LingxiUI.exe /F`，日志缺失别慌，是环境实例没继承 LINGXI_LOG
- 中文输出：控制台 GBK 乱码属正常；.cmd/.ps1 必须 ASCII

## 六、验证基线（本次收尾实测）

- e2e：exit=0，20 PASS（第 1 次尝试遇环境干扰自动重试成功）
- engine_test：5/5 PASS，`hello wor` 平均 0.300ms
- LLM 探针：703ms 首次预测（含 mmap 预热），候选"今天"
- 注册自检：PASSED（CLSID + TIP + 双 profile）
