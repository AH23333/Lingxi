#pragma once
//
// ===== 版本真源（docs/update-design-2026-11-03.md §2）=====
// DLL（ImeCore）、UI（LingxiUI）、更新逻辑、测试工装一律从这里取版本号，
// 任何其他位置禁止再硬编码版本字符串。发版时只改下面三个数字 + 两个字符串，
// 并保证与 GitHub Releases 的 tag（v<major>.<minor>.<patch>）一致。
//
// 更新检查的"是否新版"判定走语义化数值比较（major/minor/patch 三段），
// 绝不允许拿 LINGXI_VERSION_STRING 做字符串比较。
//
#define LINGXI_VERSION_MAJOR 1
#define LINGXI_VERSION_MINOR 0
#define LINGXI_VERSION_PATCH 0

// 语义化版本串。ASCII 供日志/%hs 格式；宽字符供 UI 与 --version 输出。
#define LINGXI_VERSION_STRING "1.0.0"
#define LINGXI_VERSION_WSTR   L"1.0.0"

// 构建戳：不进真源（存了也会过期），由编译器 __DATE__ __TIME__ 在各 TU 展开。
// 自查判据沿用 %TEMP%\lingxi-ime.log 的 build= 行 + ver= 行（update-design §7）。
#define LINGXI_STRW2(x) L##x
#define LINGXI_STRW(x)  LINGXI_STRW2(x)
#define LINGXI_BUILD_WSTR (LINGXI_STRW(__DATE__) L" " LINGXI_STRW(__TIME__))
