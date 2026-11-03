#pragma once
//
// ===== 自助更新：下载 / 校验 / 解压（批次 3）=====
// 设计文档：docs/update-design-2026-11-03.md。
// 覆盖威胁模型：T1（SHA256 全量比对）、T2（白名单+手动重定向，复用 update.cpp 的
// IsUrlAllowed 语义）、T4/T5/T6（zip 中央目录全量预校验 + 解出后复核）、T7（只调用
// 绝对路径 %SystemRoot%\System32\tar.exe，绝不执行包内件）、T11（超时/体积上限/可取消）。
//
// 依赖：WinHTTP + CNG(bcrypt) + 系统 tar.exe，零新增第三方库。
// 消费方：批次 4 替换流程、tools/update_test 工装、批次 6 演练。
//
#include <windows.h>

#include <string>
#include <vector>

namespace lingxi {
namespace updater {

// ===== 上限（设计 §2/§4 钉死，测试对照）=====
constexpr size_t kMaxDownloadBytes = 512ull * 1024 * 1024;          // 单包下载上限
constexpr unsigned long long kMaxFileUncompressed = 64ull * 1024 * 1024;  // 单文件解压后
constexpr unsigned long long kMaxTotalUncompressed = 256ull * 1024 * 1024; // 整包解压后总量
constexpr size_t kMaxZipEntries = 2048;                             // 条目数上限
constexpr size_t kMaxZipNameLen = 64;                               // 条目名长度上限

// ===== 下载（流式写盘；失败/取消/超限一律删除半成品）=====
// 进度回调：done/total（total=-1 表示服务端未声明 Content-Length）。返回 false 取消。
typedef bool (*DownloadProgressFn)(long long done, long long total, void* ctx);

// url 必须过 IsUrlAllowed（白名单外直接拒绝，不出网）。重定向逐跳复核（同 HttpGet）。
// destPath 的父目录必须已存在。
bool DownloadToFile(const std::wstring& url, const std::wstring& destPath, size_t maxBytes,
                    const volatile LONG* cancelFlag, DownloadProgressFn progress, void* ctx,
                    std::wstring* err);

// ===== SHA256（CNG）=====
// 整文件 → 64 位小写 hex。
bool Sha256File(const std::wstring& path, std::string* hexLower, std::wstring* err);

// 与 ParseSha256Sums 结果比对单个落盘文件：缺行/不匹配 → false + 原因。
bool VerifyAgainstSums(const std::vector<std::pair<std::wstring, std::string>>& sums,
                       const std::wstring& fileName, const std::wstring& filePath,
                       std::wstring* err);

// ===== zip 中央目录预校验（T4/T5/T6，解包前全量拒绝）=====
struct ZipEntry {
    std::string name;             // 原样条目名（校验保证为 ASCII 相对路径）
    unsigned long crc32 = 0;
    unsigned long long compSize = 0;
    unsigned long long uncompSize = 0;
    unsigned long method = 0;
    bool isDir = false;
    bool isSymlink = false;
    bool isEncrypted = false;
};

// 规则（文档 §2）：拒 `..`/`.` 段、绝对路径、盘符、`:`、`\`、控制字符、非 ASCII；
// 段不得以点/空格结尾；拒 Windows 保留设备名；拒大小写不敏感重名；
// 拒加密条目、符号链接/特殊文件（unix mode）、zip64 哨兵、非常见 OS、方法非 store/deflate；
// 单文件 >64MB、总量 >256MB、条目 >2048、名 >64 字符 → 拒。
// 允许相对子目录（如 data/pinyin.txt）。任一违规 → false，整包弃用。
bool PreflightZip(const std::wstring& zipPath, std::vector<ZipEntry>* out, std::wstring* err);

// ===== 解包（T7：绝对路径 tar.exe，超时+可取消）=====
// destDir 必须已存在。tar 退出码非 0 / 超时（120s）/ 取消 → false。
bool ExtractZipTo(const std::wstring& zipPath, const std::wstring& destDir,
                  const volatile LONG* cancelFlag, std::wstring* err);

// 解出后复核（预校验声明值可被谎报，落盘实际字节才是真相）：
// 拒符号链接（reparse point）；单文件 >64MB、总量 >256MB、文件数 >2048 → false。
bool VerifyExtractedDir(const std::wstring& dir, std::wstring* err);

} // namespace updater
} // namespace lingxi
