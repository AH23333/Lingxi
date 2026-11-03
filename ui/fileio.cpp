// fileio.cpp — 词典文件共享读写实现（批8 自 userdictwnd.cpp 迁出）
// phrases/blocked/hotphrases/设置备份随行/导出全部经 WriteFileBytes 落盘。
// 原子性口径与引擎 WriteFileAtomicA（pinyin.cpp）一致：.tmp + MoveFileEx
// 替换——UI 线程与引擎管道线程是同一文件的两个写者，读者永远不见半截文件。
// 批8 补强：CloseHandle 前 FlushFileBuffers——WriteFile 成功只进系统缓存，
// 断电/强杀下未刷盘的 .tmp 可能空或截断，刷盘后 rename 才是真原子；
// 刷盘失败按写失败处理（删 .tmp，主文件保持旧完整态）。
// 迁出独立编译单元后 engine_test 可直链本实现，实证失败路径
// （目标被排他锁/只读/陈旧 .tmp 残留——见 engine_test ROB 块）。
#include "uiinternal.h"

#include <windows.h>

namespace lingxi {

bool ReadFileBytes(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, NULL);
    // L13：GetFileSize 失败恒返回 INVALID_FILE_SIZE——不查即 resize(4GB-1)
    // 抛 length_error/bad_alloc 直接崩 UI 线程；另设 16MB 上限防异常大文件
    if (size == INVALID_FILE_SIZE || size > 16u * 1024u * 1024u)
    {
        CloseHandle(h);
        return false;
    }
    out.resize(size);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), size, &got, NULL);
    CloseHandle(h);
    out.resize(ok ? got : 0);
    return ok != FALSE;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data)
{
    // 原子写：先写 .tmp 再 MoveFileEx 替换（引擎侧 A5 同款语义）
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, NULL);
    if (ok && put == (DWORD)data.size())
        ok = FlushFileBuffers(h); // 批8：刷盘后才允许 rename 顶替主文件
    CloseHandle(h);
    if (!ok || put != (DWORD)data.size())
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace lingxi
