# 踩坑记录（PITFALLS）

> 每一条都是实际踩过、花时间排查过的坑。按"症状 → 根因 → 正确做法"记录。

## TSF / Windows 输入框架

### 1. OnSetFocus 参数顺序是反直觉的 ⭐️（最贵的一坑）
- **症状**：真实应用里切几次窗口后，候选窗再也不出现；e2e 间歇性失败
- **根因**：`ITfThreadMgrEventSink::OnSetFocus(pdimFocus, pdimPrevFocus)` —— **第一参数是"获得焦点"的文档**，第二参数才是失去焦点的。按 (old, new) 直觉命名后，焦点切换时把编辑 sink 挂到了**失去焦点**的文档上，之后的编辑全部漏掉
- **正确做法**：一律以 SDK 头文件注释为准；用 ctx 指针对照日志验证挂载目标

### 2. 键盘事件不路由：AdviseKeyEventSink 的 fForeground
- **症状**：`ITfKeystrokeMgr::TestKeyDown` 返回 S_OK/eaten=FALSE，但我们的 `OnTestKeyDown` 从未被调用
- **根因**：`AdviseKeyEventSink(tid, sink, fForeground)` 第三参数传 FALSE（照抄官方样本）时，CTF 只把按键路由给"前台键 sink"；程序化激活的宿主里 CTF 不会自动设置
- **正确做法**：直通+预测形态必须传 TRUE（激活期间要看全部按键；Deactivate 会解除注册，不影响其他输入法）

### 3. 现代 SDK 与老样本的差异
- `ITfEditSession` 回调方法名是 **`DoEditSession`**（不是 OnEditSession）
- `ITfTextEditSink::OnEndEdit` 签名是 `(pic, TfEditCookie ecReadOnly, ITfEditRecord*)`——老 Win7 样本里的 `(pic, fReadOnly, ecOld, ecNew)` 编译不过
- `TS_SS_DISABLED` 不存在（TS_SS_* 只有 DISJOINTSEL/REGIONS/TRANSITORY 等）。密码框隔离由 TSF 框架层完成：应用禁用 IME 的控件根本不产生事件，无需自查
- 官方样本 Register.cpp 里的 QueryInterface 有 bug（cast 成 `ITfLangBarItemButton*`），照抄会翻车，自己写对

### 4. CTF 注册三连坑
- **非管理员**：CLSID 可以 per-user（HKCU），但 `ITfInputProcessorProfiles::Register` / `RegisterCategory` 只写 HKLM，无管理员一律 **E_FAIL**（不是 E_ACCESSDENIED，极具迷惑性）
- **路径含空格**：InprocServer32 默认值必须写 `"引号包裹的路径"`，否则 COM/CTF 解析截断 → Register 失败
- **键名**：注册表真实位置是 `HKLM\SOFTWARE\Microsoft\CTF\TIP\{clsid}`——**TIP 是单数**（网上资料多写成 TIPs）

### 5. transitory 上下文只支持同步编辑会话
- **症状**：在（经 CUAS 的）非 TSF-aware 应用里上下文读取/上屏静默失败
- **根因**：`TF_ES_ASYNCDONTCARE` 在 transitory 上下文不被接受
- **正确做法**：所有 `RequestEditSession` 用"**同步优先**，返回 TS_E_SYNCHRONOUS 再降级 ASYNCDONTCARE"

### 6. ctfmon 会抢回前台输入状态
- **症状**：程序化 `ActivateLanguageProfile` 成功后几百毫秒，`KeySink OnSetFocus(foreground=0)` 被触发，按键路由失效
- **根因**：ctfmon 监视窗口焦点，周期性为前台窗口应用系统保存的输入法
- **对策**：e2e 等自动化场景需要周期性重新激活对抗；真实用户走 Win+Space 切换器则无此问题（系统自己维护）

### 7. 合成宿主的三明治上下文
- **症状**：e2e 断言读自己的 ITextStoreACP 拿不到 IME 上屏的文本
- **根因**：CTF 可能往文档栈推镜像/三明治上下文，IME 的编辑落在镜像里
- **正确做法**：测试断言一律**从焦点文档顶层上下文经 TSF 读文本**（这才是用户看到的），而不是读自己的存储

### 8. 合成宿主必须有真实窗口 + AssociateFocus
- 只 `SetFocus(docMgr)` 不够：CTF 键盘路由依赖窗口焦点状态。官方 tsfapp 用 `ITfThreadMgr::AssociateFocus(hwnd, docMgr, &prev)`——照做才有按键路由

## 进程与工程

### 9. TSF DLL 的文件锁：能改名、不能覆盖
- **症状**：MSBuild LNK1168 无法写 ImeCore.dll（明明没有任何"我们的"进程在跑）
- **根因**：CTF 把 TIP DLL 加载进所有激活了它的进程（包括正在用的编辑器）；已加载 DLL 禁止覆盖写，但**允许改名**
- **正确做法**：build.cmd 构建前把旧产物 `ren` 为 `.prev` 让位，链接器永远写全新文件；注册表指向路径不变

### 10. 命名管道多线程写帧交错
- **症状**：间歇性消息丢失/解析失败（HELLO/ACTIVATE 偶发消失）
- **根因**：DLL 侧发送来自两个线程（管道读线程发 ACTIVATE、宿主 UI 线程发 CTX），`[u32 len][type][body]` 帧被并发写交错
- **正确做法**：同一连接的所有 WriteFrame 全程持同一把锁

### 11. 先连接后补发状态快照
- **症状**：UI 进程永远收不到 ACTIVATE，候选窗不显示
- **根因**：ACTIVATE 在 `ActivateEx` 里发，那时管道还没连上
- **正确做法**：连接建立后（HELLO 一起）补发 ACTIVATE 快照；同时候选窗跟随"最后发 CTX 的客户端"（正在打字者）而非最后发 ACTIVATE 者

## 工具链

### 12. cmd.exe 批处理与 PowerShell 5.1 脚本都按 ANSI(GBK) 读取
- UTF-8 中文注释的 .cmd 直接解析爆炸（乱码命令）；.ps1 无 BOM 时中文字符串解析报错
- **正确做法**：.cmd 纯 ASCII；.ps1 纯 ASCII 或加 UTF-8 BOM

### 13. 日志文件是 UTF-16LE
- LogLine 直接 `WriteFile(wchar_t 缓冲)`，文件为 UTF-16LE——用 `iconv -f UTF-16LE` 或支持该编码的查看器读，Read 工具会当二进制拒读

### 14. MSBuild 的 PostBuildEvent 是属性不是项
- 放进 `ItemGroup` 报 MSB4232；必须在 `<ItemDefinitionGroup>` 内

### 15. 并发构建互相锁文件
- 多个子代理并发跑同一 solution 构建会互锁 obj/bin → 每个测试工程用私有 OutDir（如 `bin\enginetest\`），且不要把测试工程加进主 sln

## llama.cpp（M4 新增）

### 16. 版本漂移防御
- 采样器 API（llama_sampler_*）跨版本变动大；`llama_decode` + `llama_get_logits_ith` + 自己 argmax 是十年稳定路径
- 结构体参数（model/context params）**必须**用与二进制同 tag 的头文件编译，否则字段偏移漂移导致写错字段
- 运行时 LoadLibrary + GetProcAddress 绑定，任一符号缺失 → 引擎整体降级，绝不崩溃
- LoadLibrary 加载带依赖的 llama.dll 要用 `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`，否则 ggml.dll 等依赖按默认搜索路径找不到
