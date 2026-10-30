#pragma once
//
// 轻量日志：
//   LX_LOG  —— 关键/低频事件：始终 OutputDebugStringW（DebugView 可见）；
//              设置 LINGXI_LOG=1 时同时追加到 %TEMP%\lingxi-ime.log。
//   LX_LOGV —— 热路径高频诊断（逐候选、逐路径、逐键）：未设置 LINGXI_LOG=1
//              时【零开销】，宏做 if 短路，连参数表达式（如 ToUtf8 转换）
//              都不求值。门控读取用常量初始化的 static int，避免 magic
//              static 在 Debug 下的运行时线程安全检查。
//
// 日志绝不含上下文文本内容（仅计数/状态）。
//
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace lingxi {

// 热路径门控：常量初始化为 false（无动态初始化守卫，读取即普通内存读）。
// 进程启动时由 InitLogging() 按 LINGXI_LOG 环境变量置位。
inline bool g_lingxiVerbose = false;

// 启动时调用：读环境变量打开文件日志（UI 的 main、DLL 的 DllMain）。
inline void InitLogging()
{
    wchar_t f[8] = L"";
    if (GetEnvironmentVariableW(L"LINGXI_LOG", f, 8) > 0 && f[0] == L'1')
        g_lingxiVerbose = true;
}

// 文件写入需要的锁（仅在真正写日志时构造，低频路径）。
struct FileLogState
{
    CRITICAL_SECTION cs;
    bool csInit = false;
    FileLogState() { InitializeCriticalSection(&cs); csInit = true; }
    ~FileLogState() { if (csInit) DeleteCriticalSection(&cs); }
};

inline FileLogState& GetFileLogState()
{
    static FileLogState state;
    return state;
}

// 把格式化内容追加到日志文件（调用方已判 enabled）
inline void AppendToFile(const wchar_t* line)
{
    if (!g_lingxiVerbose) return;
    EnterCriticalSection(&GetFileLogState().cs);
    wchar_t temp[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp) > 0)
    {
        _snwprintf_s(path, _TRUNCATE, L"%slingxi-ime.log", temp);
        // 轮转：超 1MB 改名 .old 留档重开——长期开着 LINGXI_LOG=1 诊断时防
        // 无界增长吃满 %TEMP% 配额；失败不阻塞追加（继续写当前文件）。
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeLow > 1024u * 1024u)
        {
            wchar_t oldPath[MAX_PATH];
            _snwprintf_s(oldPath, _TRUNCATE, L"%s.old", path);
            DeleteFileW(oldPath);
            MoveFileW(path, oldPath);
        }
        HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, NULL);
            CloseHandle(h);
        }
    }
    LeaveCriticalSection(&GetFileLogState().cs);
}

// 时间戳前缀（事件间相对时延排查：响应往返、等待窗口覆盖性）
inline void GetLogTimeStamp(wchar_t* ts, size_t cch)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(ts, cch, _TRUNCATE, L"%02u:%02u:%02u.%03u",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

// 关键日志：始终 OutputDebugStringW；LINGXI_LOG=1 时额外写文件。
inline void LogLine(const wchar_t* tag, const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t ts[16];
    GetLogTimeStamp(ts, 16);
    wchar_t line[600];
    _snwprintf_s(line, _TRUNCATE, L"[%s][Lingxi][%s] %s\n", ts, tag, buf);
    OutputDebugStringW(line);
    AppendToFile(line);
}

// 热路径诊断日志：仅在开启文件日志时由 LX_LOGV 宏调用（宏已门控，
// 关闭时参数表达式不求值）。
inline void LogLineV(const wchar_t* tag, const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t ts[16];
    GetLogTimeStamp(ts, 16);
    wchar_t line[600];
    _snwprintf_s(line, _TRUNCATE, L"[%s][Lingxi][%s] %s\n", ts, tag, buf);
    AppendToFile(line);
}

} // namespace lingxi

#define LX_LOG(tag, ...) lingxi::LogLine(tag, __VA_ARGS__)
// 直接读常量初始化标志（无函数 call、无动态初始化守卫）；if 短路保证关闭时
// 连 __VA_ARGS__（如 ToUtf8 转换）都不求值。
#define LX_LOGV(tag, ...) do { if (lingxi::g_lingxiVerbose) lingxi::LogLineV(tag, __VA_ARGS__); } while (0)
