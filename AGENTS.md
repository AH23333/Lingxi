# AGENTS.md — 会话首读（工程纪律与入口）

> P0 立规（docs/ARCH-ASSESSMENT.md §五，2026-10-31 随 P1-P3 重构落地）。
> 全量流水见 docs/DEVLOG.md；动手前先查 docs/PITFALLS.md（#1-#94，多数"程序坏了"实为环境/工装坑）。
> 2026-11-02 只读深度 Bug 审查结论（高 H1-H6/中 M1-M7/低 L1-L13，含证据、复现状态与已排除项清单）：docs/audit-2026-11-02-bug-audit.md——排查崩溃/竞态/乱码类问题先查此表，勿重复排查"已排除项"。2026-11-02 晚状态已按修复代码对齐；2026-11-03 复核销账 L10（`53f0aaf` 封顶淘汰）/L12（`bda5094`）并确认 H3 根治闭环（`b8d1d8d` 停机不析构+落盘线程去 this 化）；同日 L11 复探针入门禁（`8fbd016`，§八-5 销账）、L10 边界单测入门禁（`485727a`）——26 条全部有修复落地且有验证（H5 原判定撤回；仅 L6/L7/L8 待真机行为验证）。残余风险见该文档 §八-4/-6/-7 与 §7.5 复核增补（§八-3 M5 TTL 窗口、§八-8 冷启动竞态已销账）。

## 构建与门禁入口

- 构建：`cmd /c "tools\build.cmd [Debug|Release]"`（x64 UI+ImeCore）；`cmd /c "tools\build32.cmd"`（Win32 ImeCore）
- 全量回归：`cmd /c tools\regress.cmd`（engine_test 313 断言 / fix_readings --check / deep_test / e2e 四套件，**ALL PASS 才算过**；沙箱内 `cmd /c` 被禁时按 §PITFALLS 用 MSBuild 直调 + 逐套件手跑等效）
- 真机走查：`tmp\smoke1.ps1`（16 项，含 usage.txt 播种备份/还原）+ `tmp\smoke2.ps1`（8 项）；行为探针 `LingxiUI.exe --probe-pinyin`（基线：gylg=4 候 / gylgl=供应链管理 1 候 / xian 首选西安）
- 提交纪律：build 零错 + regress ALL PASS；涉 UI/窗口/焦点的改动加跑 smoke 双套件；重构类改动加跑 probe 基线

## ui 层文件纪律（P0 立规——防拆分成果回胖）

1. **新增窗口/功能一律新文件**（usagestats.cpp 是正确示范），挂入 LingxiUI.vcxproj。
2. **settingsui.cpp 只准减不准增**（P1 拆分后 1796 行，此为回归上界）。
3. 同一类多 TU 合法且推荐复用：candwnd_core/render/interact 共享 candwnd_internal.h 的三文件模式；但此类内部头**不得泄漏出对应文件族**。
4. **uiinternal.h 只限拆分文件族 + main.cpp**（g_hooks 装配）使用；对外公共 API 只走 window.h / settingsui.h 等公共头。
5. **跨文件通知一律挂 lingxi::g_hooks（uiinternal.h 的 UiHooks）**：新增通知加结构体字段 + main.cpp 装配段填充，**禁止再造文件级全局 + SetXxxCallback**（P3 已退役 6 组）。
6. 文件级共享资源（画刷/字体/图标）声明走 uiinternal.h，定义留 settingsui.cpp / tray.cpp 原位；各文件私有状态一律匿名命名空间。

## 环境注意（Windows / PowerShell 5.1）

- git/msbuild 经 `cmd /c` 执行；rg/head 等 Unix 工具不可用（搜索用 Select-String 或专用 Grep 工具）。
- 含中文的 .ps1 必须 UTF-8 带 BOM，否则中文字面串静默乱码、全量检查假失败（PITFALLS #65）。
- 宿主 LingxiUI.exe 常驻且会被 7+ 进程内的 IME DLL 断连自动重生（#66）——行为验证优先窗口枚举/注册表/文件探针，勿依赖进程环境变量注入的日志。
- 无人值守测试宿主涉及真实焦点路由时必须 AttachThreadInput 抢前台（e2e.cpp ForceForeground，#67）。
