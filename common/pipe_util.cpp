#include "pipe_util.h"

#include <sddl.h>
#include <aclapi.h>

namespace lingxi {

// 管道 DACL 限定为当前用户，避免同机其他账户连接。
static SECURITY_ATTRIBUTES* GetUserSA()
{
    static SECURITY_ATTRIBUTES sa;
    static PSECURITY_DESCRIPTOR pSD = nullptr;
    static bool s_init = false;
    if (!s_init)
    {
        s_init = true;
        HANDLE hToken = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        {
            DWORD len = 0;
            GetTokenInformation(hToken, TokenUser, NULL, 0, &len);
            if (len > 0)
            {
                auto pUser = (TOKEN_USER*)LocalAlloc(LPTR, len);
                if (pUser && GetTokenInformation(hToken, TokenUser, pUser, len, &len))
                {
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(pUser->User.Sid, &sidStr))
                    {
                        wchar_t sddl[256];
                        _snwprintf_s(sddl, _TRUNCATE, L"D:(A;;GA;;;%s)", sidStr);
                        ConvertStringSecurityDescriptorToSecurityDescriptorW(
                            sddl, SDDL_REVISION_1, &pSD, NULL);
                        LocalFree(sidStr);
                    }
                }
                LocalFree(pUser);
            }
            CloseHandle(hToken);
        }
    }
    if (!pSD) return nullptr; // 回退默认 DACL
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;
    return &sa;
}

HANDLE CreateLingxiPipeServerInstance(bool firstInstance)
{
    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (firstInstance) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

    SECURITY_ATTRIBUTES* pSA = GetUserSA();
    HANDLE h = CreateNamedPipeW(kPipeName,
                                openMode,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES,
                                64 * 1024, 64 * 1024,
                                0, pSA);
    return h;
}

HANDLE OpenLingxiPipeClient()
{
    SECURITY_ATTRIBUTES* pSA = GetUserSA();
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE,
                           0, pSA, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        // 服务端实例可能刚被占满，等一下再试一次。
        if (GetLastError() == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(kPipeName, 500);
            h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE,
                            0, pSA, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        }
    }
    return h;
}

static bool WaitOverlapped(HANDLE hPipe, OVERLAPPED& ov, HANDLE hStop, uint32_t timeoutMs,
                           DWORD* pTransferred, bool* pStopped)
{
    *pStopped = false;
    HANDLE waits[2] = {ov.hEvent, hStop};
    DWORD n = hStop ? 2 : 1;
    DWORD w = WaitForMultipleObjects(n, waits, FALSE, timeoutMs);
    if (w == WAIT_OBJECT_0)
    {
        return GetOverlappedResult(hPipe, &ov, pTransferred, FALSE) != FALSE;
    }
    if (hStop && w == WAIT_OBJECT_0 + 1)
    {
        *pStopped = true;
        CancelIoEx(hPipe, &ov);
        return false;
    }
    CancelIoEx(hPipe, &ov); // 超时
    return false;
}

bool WriteFrame(HANDLE hPipe, uint8_t type, const void* body, uint32_t bodyLen, uint32_t timeoutMs)
{
    if (hPipe == INVALID_HANDLE_VALUE || hPipe == NULL) return false;

    std::vector<uint8_t> frame;
    frame.reserve(sizeof(uint32_t) + 1 + bodyLen);
    uint32_t total = 1 + bodyLen;
    const uint8_t* p = (const uint8_t*)&total;
    frame.insert(frame.end(), p, p + sizeof(total));
    frame.push_back(type);
    if (bodyLen)
    {
        p = (const uint8_t*)body;
        frame.insert(frame.end(), p, p + bodyLen);
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    bool ok = false;
    if (WriteFile(hPipe, frame.data(), (DWORD)frame.size(), NULL, &ov) ||
        GetLastError() == ERROR_IO_PENDING)
    {
        DWORD transferred = 0;
        bool stopped = false;
        ok = WaitOverlapped(hPipe, ov, NULL, timeoutMs, &transferred, &stopped);
        ok = ok && transferred == frame.size();
    }
    CloseHandle(ov.hEvent);
    return ok;
}

bool WriteFrameStr(HANDLE hPipe, uint8_t type, const void* prefixBody, uint32_t prefixLen,
                   const std::wstring& str, uint32_t timeoutMs)
{
    std::vector<uint8_t> body;
    if (prefixLen)
    {
        const uint8_t* p = (const uint8_t*)prefixBody;
        body.insert(body.end(), p, p + prefixLen);
    }
    AppendStr(body, str);
    return WriteFrame(hPipe, type, body.data(), (uint32_t)body.size(), timeoutMs);
}

bool ReadFrame(HANDLE hPipe, HANDLE hStop, uint8_t& type, std::vector<uint8_t>& body)
{
    std::vector<uint8_t> accum;
    uint32_t need = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;
    uint8_t buf[4096];

    auto readMore = [&](DWORD want) -> int
    {
        // 返回 1=有进展, 0=停止, -1=出错
        if (accum.size() < want)
        {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (ReadFile(hPipe, buf, (DWORD)min(sizeof(buf), want - (DWORD)accum.size() + 64),
                         NULL, &ov) || GetLastError() == ERROR_IO_PENDING)
            {
                DWORD transferred = 0;
                bool stopped = false;
                if (!WaitOverlapped(hPipe, ov, hStop, INFINITE, &transferred, &stopped))
                    return stopped ? 0 : -1;
                if (transferred == 0) return -1; // 管道关闭
                accum.insert(accum.end(), buf, buf + transferred);
                return 1;
            }
            return -1;
        }
        return 1;
    };

    bool ok = false;
    for (;;)
    {
        if (accum.size() < sizeof(uint32_t))
        {
            int r = readMore(sizeof(uint32_t));
            if (r != 1) break;
            continue;
        }
        uint32_t total = 0;
        memcpy(&total, accum.data(), sizeof(total));
        if (total < 1 || total > 1 * 1024 * 1024) break; // 非法帧
        if (accum.size() < sizeof(uint32_t) + total)
        {
            int r = readMore(sizeof(uint32_t) + total);
            if (r != 1) break;
            continue;
        }
        type = accum[sizeof(uint32_t)];
        body.assign(accum.begin() + sizeof(uint32_t) + 1, accum.begin() + sizeof(uint32_t) + total);
        ok = true;
        break;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

} // namespace lingxi
