#pragma once
//
// 轻量日志：OutputDebugStringW 始终输出（DebugView 可见）；
// 设置环境变量 LINGXI_LOG=1 时，同时追加写入 %TEMP%\lingxi-ime.log，
// 便于不接调试器时排查 TSF 行为。日志绝不含上下文文本内容。
//
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace lingxi {

inline void LogLine(const wchar_t* tag, const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);

    wchar_t line[600];
    _snwprintf_s(line, _TRUNCATE, L"[Lingxi][%s] %s\n", tag, buf);
    OutputDebugStringW(line);

    static bool s_checked = false;
    static bool s_fileEnabled = false;
    static CRITICAL_SECTION s_cs;
    if (!s_checked)
    {
        // 无竞争风险的惰性初始化：最坏情况是两个线程各自初始化一次。
        s_checked = true;
        wchar_t flag[8] = L"";
        if (GetEnvironmentVariableW(L"LINGXI_LOG", flag, 8) > 0 && flag[0] == L'1')
        {
            InitializeCriticalSection(&s_cs);
            s_fileEnabled = true;
        }
    }
    if (s_fileEnabled)
    {
        EnterCriticalSection(&s_cs);
        wchar_t temp[MAX_PATH];
        wchar_t path[MAX_PATH];
        if (GetTempPathW(MAX_PATH, temp) > 0)
        {
            _snwprintf_s(path, _TRUNCATE, L"%slingxi-ime.log", temp);
            HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, NULL);
                CloseHandle(h);
            }
        }
        LeaveCriticalSection(&s_cs);
    }
}

} // namespace lingxi

#define LX_LOG(tag, ...) lingxi::LogLine(tag, __VA_ARGS__)
