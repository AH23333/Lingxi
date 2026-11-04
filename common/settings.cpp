#include "settings.h"

#include <windows.h>
#include <cwchar>

namespace lingxi {

namespace {

// 真实设置基键（用户环境）
constexpr wchar_t kKeyReal[] = L"Software\\LingxiIme";

// M2：非法/超长 ns 的隔离回退键——绝不静默回退真实键（防测试进程污染用户设置）
constexpr wchar_t kKeyQuarantine[] = L"Software\\LingxiIme_NS_INVALID";

// 测试隔离：LINGXI_SETTINGS_NS 非空（仅 [A-Za-z0-9_-]，上限 47 字符）
// → 读写 Software\LingxiIme_<ns> 沙盒基键；非法/超长 ns → 隔离键
// NS_INVALID + 显式报错，永不触碰真实键（M2）。
// 背景（2026-10-29 晚实证）：全套测试此前直写真实 HKCU——设置保存是
// "读当前值改单键全量回写"链路的任何一环在测试进程里崩溃/半程退出都会把
// 用户实时设置刷成残值（本轮就发生过一次：全部能力位被刷 0，用户输入法
// 直接被关掉拼音）。词典有 APPDATA 沙盒、注册表此前没有等价物，现在补上。

// M2：ns 合法性统一判定——ActiveKey（实际写哪个键）与 TestSpawnedFromEnv
// （是否按测试派生行事）必须同口径。旧实现前者逐字符校验、后者只看非空，
// 非法 ns（如含空格/中文）下分裂：判定为测试派生却写真实键。
static bool NsValid(const wchar_t* ns, size_t len)
{
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++)
    {
        wchar_t c = ns[i];
        bool okSym = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                     (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
        if (!okSym) return false;
    }
    return true;
}

const wchar_t* ActiveKey()
{
    wchar_t ns[64] = L"";
    DWORD n = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", ns, 48);
    if (n == 0) return kKeyReal; // 未设置/空 = 生产
    // M2：非法/超长 ns 不再静默回退真实键（旧行为与 TestSpawnedFromEnv 分裂，
    // "自认测试"的进程把设置刷进真实键）。现显式报错一次 + 降级到固定隔离键，
    // 读写永不落真实键。
    if (n >= 48 || !NsValid(ns, n))
    {
        static LONG s_errLogged = 0;
        if (InterlockedExchange(&s_errLogged, 1) == 0)
            OutputDebugStringW(
                L"LingxiIme: LINGXI_SETTINGS_NS invalid/overlong; settings quarantined "
                L"to Software\\LingxiIme_NS_INVALID\n");
        return kKeyQuarantine;
    }
    static wchar_t buf[128];
    _snwprintf_s(buf, _TRUNCATE, L"Software\\LingxiIme_%s", ns);
    return buf;
}

bool GetDword(HKEY hKey, const wchar_t* name, DWORD& out)
{
    DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_DWORD, &type, &v, &cb) == ERROR_SUCCESS &&
        type == REG_DWORD)
    {
        out = v;
        return true;
    }
    return false;
}

void SetDword(HKEY hKey, const wchar_t* name, DWORD v)
{
    RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

bool GetQword(HKEY hKey, const wchar_t* name, unsigned long long& out)
{
    unsigned long long v = 0;
    DWORD cb = sizeof(v), type = 0;
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_QWORD, &type, &v, &cb) == ERROR_SUCCESS &&
        type == REG_QWORD) { out = v; return true; }
    return false;
}

void SetQword(HKEY hKey, const wchar_t* name, unsigned long long v)
{
    RegSetValueExW(hKey, name, 0, REG_QWORD, (const BYTE*)&v, sizeof(v));
}

// M5：设置版本号 bump——SaveSettings/SaveSettingsDiff 落盘尾部调用；读侧
// （LoadSettings）版本不变且未超 TTL 才用缓存。键删除（ResetToDefaults）
// 连版本号一起消失 → 下次读必 miss。
// 双写碰撞的真实兜底口径（N11 更正旧注释误述"下一次 bump 自愈"）：先读后加
// 非原子，双写者可产生**相同**新 seq——碰撞双方已把 seq 推到同值，读侧此后
// 缓存命中不再重读，"下一次 bump"并不存在。实际自愈靠读侧 kSetCacheTtlMs
// （500ms）TTL：撕裂/陈旧快照最多存活一个 TTL 周期，过期全量重读即收敛。
// 不改行为的理由：生产单写者（LingxiUI 单例 + IME DLL 只读设置）使碰撞实际
// 不可达；跨进程 CAS 需命名互斥锁，复杂度/挂死风险换不来不可达场景的收益。
// engine_test N11 块钉 TTL 自愈语义（同 seq 残局 → TTL 内陈旧、过期收敛）。
void BumpSettingsSeq(HKEY hKey)
{
    DWORD seq = 0;
    GetDword(hKey, L"SettingsSeq", seq);
    SetDword(hKey, L"SettingsSeq", seq + 1);
}

// REG_SZ 读（上限 2KB 防异常大值；尾部 NUL 剥离）
bool GetString(HKEY hKey, const wchar_t* name, std::wstring& out)
{
    DWORD cb = 0;
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &cb) != ERROR_SUCCESS ||
        cb == 0 || cb > 2048)
        return false;
    // L3：cb 为字节数（含结尾 NUL）——注册表对齐到 4 字节时可能为奇数，
    // cb/2 向下取整会少一个字符装不下数据，按 (cb+1)/2 向上取整
    std::wstring buf((cb + 1) / sizeof(wchar_t), L'\0');
    if (RegGetValueW(hKey, NULL, name, RRF_RT_REG_SZ, NULL,
                     (LPBYTE)&buf[0], &cb) != ERROR_SUCCESS)
        return false;
    size_t len = wcslen(buf.c_str()); // 数据可能带 NUL 填充，按 C 串截
    out = buf.substr(0, len);
    return true;
}

void SetString(HKEY hKey, const wchar_t* name, const std::wstring& v)
{
    RegSetValueExW(hKey, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                   (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

// 测试派生判定（与 ActiveKey 同口径：ns 非空即测试派生；非法/超长 ns 的
// 设置由 ActiveKey 落隔离键，两侧仍一致）：三态缓存，
// -1 未快照（惰性读环境） 0 生产 1 测试派生。
int g_testSpawned = -1;

bool TestSpawnedFromEnv()
{
    wchar_t ns[48] = L"";
    DWORD n = GetEnvironmentVariableW(L"LINGXI_SETTINGS_NS", ns, 48);
    if (n == 0) return false;
    // M2：ns 存在（非空）即测试派生——非法/超长 ns 的设置读写落在隔离键
    // （而非真实键），"自认测试却写真实键"不再存在；剪贴板/备份/统计等
    // 沙箱语义取保守方向（停用真实写入）。
    return true;
}

} // namespace

bool SnapshotTestSpawned()
{
    g_testSpawned = TestSpawnedFromEnv() ? 1 : 0;
    return g_testSpawned != 0;
}

bool WasTestSpawned()
{
    if (g_testSpawned < 0)
        g_testSpawned = TestSpawnedFromEnv() ? 1 : 0;
    return g_testSpawned != 0;
}

namespace {
// M5：击键热路径全量读缓存——ime composition/service 每键全量 LoadSettings
// （~35 次 RegQueryValueEx + 2 次两段式字符串读），注册表 API 在宿主内为
// 50ms 级抖动源。失效判据=键内 SettingsSeq 版本号：本进程/他进程的
// SaveSettings/SaveSettingsDiff 落盘后 bump → 下一次读即 miss 重读（"改了
// 设置不生效"不存在）。不用键 LastWriteTime 做判据：实测其粒度为秒级
// （同秒内连续写时间戳不变），不可靠。外部直写单键（reg 导入、deep_test
// 类工具）不 bump → 由 TTL 兜底：缓存最多存活 kSetCacheTtlMs，过期即全量
// 重读，陈旧有界。键不存在（全新环境/恢复默认）走原直读路径。无痕开关另
// 走 IncognitoActive() 单键直读，不受缓存影响。
constexpr ULONGLONG kSetCacheTtlMs = 500;
SRWLOCK g_setCacheLock = SRWLOCK_INIT;
bool g_setCacheValid = false;
bool g_setCacheHaveSeq = false;
DWORD g_setCacheSeq = 0;
ULONGLONG g_setCacheTick = 0;
std::wstring g_setCacheKey;
Settings g_setCache;
} // namespace

Settings LoadSettings()
{
    Settings s;
    const std::wstring key = ActiveKey(); // 本地快照：ActiveKey 返回内部 static buf 指针，缓存路径需跨全函数持有
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return s; // 缺键 = 全默认

    DWORD seq = 0;
    const bool haveSeq = GetDword(hKey, L"SettingsSeq", seq);
    {
        AcquireSRWLockShared(&g_setCacheLock);
        const bool hit = g_setCacheValid && g_setCacheKey == key &&
                         g_setCacheHaveSeq == haveSeq && (!haveSeq || g_setCacheSeq == seq) &&
                         GetTickCount64() - g_setCacheTick < kSetCacheTtlMs;
        if (hit) s = g_setCache;
        ReleaseSRWLockShared(&g_setCacheLock);
        if (hit) { RegCloseKey(hKey); return s; }
    }

    DWORD v = 0;
    if (GetDword(hKey, L"PinyinEnabled", v)) s.pinyinEnabled = v != 0;
    if (GetDword(hKey, L"CandidateCount", v) && v >= 1 && v <= 9) s.candidateCount = v;
    if (GetDword(hKey, L"CandPageSize", v) && v >= 1 && v <= 9) s.candPageSize = v;
    if (GetDword(hKey, L"CandidateFontSize", v) && v >= 12 && v <= 32) s.candidateFontSize = v;
    if (GetDword(hKey, L"CandOrientation", v) && v <= 1) s.candOrientation = v;
    if (GetDword(hKey, L"DecayEnabled", v) && v <= 1) s.decayEnabled = v;
    if (GetDword(hKey, L"HalfLifeDays", v) && v >= 7 && v <= 365) s.decayHalfLifeDays = v;
    if (GetDword(hKey, L"DarkMode", v) && v <= 2) s.darkMode = v;
    if (GetDword(hKey, L"EnCompletion", v)) s.enCompletion = v != 0;
    if (GetDword(hKey, L"Paused", v)) s.paused = v != 0;
    if (GetDword(hKey, L"Incognito", v)) s.incognito = v != 0;
    if (GetDword(hKey, L"CnPunct", v)) s.cnPunct = v != 0;
    if (GetDword(hKey, L"FullWidthAlnum", v)) s.fullWidthAlnum = v != 0;
    if (GetDword(hKey, L"FuzzyPinyin", v)) s.fuzzyPinyin = v != 0;
    if (GetDword(hKey, L"FuzzyGroups", v) && v <= 0xFF) s.fuzzyGroups = v; // 缺键=全组，与历史行为一致
    // 中英切换键：新键 ToggleMode 优先；无/越界则旧键 ShiftToggle
    // （bool）派生 0/1；两键均缺 = 默认 1（Shift 切换，与历史行为一致）
    DWORD tm = 0;
    if (GetDword(hKey, L"ToggleMode", tm) && tm <= 4)
        s.toggleMode = tm;
    else if (GetDword(hKey, L"ShiftToggle", v))
        s.toggleMode = v != 0 ? 1 : 0;
    if (GetDword(hKey, L"Shuangpin", v)) s.shuangpin = v != 0;
    if (GetDword(hKey, L"ShuangpinScheme", v) && v <= 2) s.shuangpinScheme = v;
    if (GetDword(hKey, L"SymbolHotkey", v)) s.symbolHotkey = v != 0;
    if (GetDword(hKey, L"Traditional", v)) s.traditionalOutput = v != 0;
    if (GetDword(hKey, L"LlmRerank", v)) s.llmRerank = v != 0;
    if (GetDword(hKey, L"LlmMultiPath", v)) s.llmMultiPath = v != 0;
    if (GetDword(hKey, L"SkinIndex", v) && v <= 6) s.skinIndex = v;
    if (GetDword(hKey, L"UiFont", v) && v <= 3) s.uiFont = v;
    if (GetDword(hKey, L"CandidateOpacity", v) && v >= 70 && v <= 100) s.candidateOpacity = v;
    if (GetDword(hKey, L"AssocAfterPick", v)) s.assocAfterPick = (v != 0);  // 缺键=结构默认(true)
    if (GetDword(hKey, L"CandidatePinned", v)) s.candidatePinned = (v != 0 ? 1u : 0u);
    if (GetDword(hKey, L"CandidatePinX", v)) s.candidatePinX = (int)v; // 负值经 DWORD 往返
    if (GetDword(hKey, L"CandidatePinY", v)) s.candidatePinY = (int)v;
    GetString(hKey, L"CellDisabled", s.cellDisabled); // A 细胞词库停用表；缺键=空=全启用
    GetString(hKey, L"RecentSymbols", s.recentSymbols); // 缺键=空（"最近"页空态）
    GetString(hKey, L"FavoriteSymbols", s.favoriteSymbols); // 缺键=空（"收藏"页空态）
    if (GetDword(hKey, L"DailyCharGoal", v) && v <= 100000) s.dailyCharGoal = v;
    if (GetDword(hKey, L"LastSettingsPage", v)) s.lastSettingsPage = v; // 越界钳制在消费方（页数随版本变）
    if (GetDword(hKey, L"LastSymbolCat", v)) s.lastSymbolCat = v;
    if (GetDword(hKey, L"SetWinX", v)) s.setWinX = (int)v; // 负值经 DWORD 往返
    if (GetDword(hKey, L"SetWinY", v)) s.setWinY = (int)v;
    if (GetDword(hKey, L"SetWinW", v)) s.setWinW = (int)v;
    if (GetDword(hKey, L"SetWinH", v)) s.setWinH = (int)v;
    GetString(hKey, L"UpdateRepo", s.updateRepo);
    if (GetDword(hKey, L"UpdateAutoCheck", v)) s.updateAutoCheck = v ? 1u : 0u;
    GetQword(hKey, L"UpdateLastCheck", s.updateLastCheck);

    RegCloseKey(hKey);
    AcquireSRWLockExclusive(&g_setCacheLock);
    g_setCache = s;
    g_setCacheKey = key;
    g_setCacheSeq = seq;
    g_setCacheHaveSeq = haveSeq;
    g_setCacheTick = GetTickCount64();
    g_setCacheValid = true;
    ReleaseSRWLockExclusive(&g_setCacheLock);
    return s;
}

// 无痕模式快速查询：单键读（选词热路径 µs 级；缺键=关）。
// 语义与 LoadSettings().incognito 一致；托盘开关落盘后下一选词即生效。
bool IncognitoActive()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD v = 0;
    bool active = GetDword(hKey, L"Incognito", v) && v != 0;
    RegCloseKey(hKey);
    return active;
}

void SaveSettings(const Settings& s)
{
    HKEY hKey = nullptr;
    // M5：BumpSettingsSeq 需先读旧 seq，打开权限从 KEY_WRITE 提到 KEY_READ|KEY_WRITE
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;

    SetDword(hKey, L"PinyinEnabled", s.pinyinEnabled ? 1 : 0);
    SetDword(hKey, L"CandidateCount", s.candidateCount);
    SetDword(hKey, L"CandPageSize", s.candPageSize);
    SetDword(hKey, L"CandidateFontSize", s.candidateFontSize);
    SetDword(hKey, L"CandOrientation", s.candOrientation);
    SetDword(hKey, L"DecayEnabled", s.decayEnabled);
    SetDword(hKey, L"HalfLifeDays", s.decayHalfLifeDays);
    SetDword(hKey, L"DarkMode", s.darkMode);
    SetDword(hKey, L"EnCompletion", s.enCompletion ? 1 : 0);
    SetDword(hKey, L"Paused", s.paused ? 1 : 0);
    SetDword(hKey, L"Incognito", s.incognito ? 1 : 0);
    SetDword(hKey, L"CnPunct", s.cnPunct ? 1 : 0);
    SetDword(hKey, L"FullWidthAlnum", s.fullWidthAlnum ? 1 : 0);
    SetDword(hKey, L"FuzzyPinyin", s.fuzzyPinyin ? 1 : 0);
    SetDword(hKey, L"FuzzyGroups", s.fuzzyGroups);
    // 双键兼容：ShiftToggle（旧 bool）与新 ToggleMode 同步写——旧版 UI/DLL 读旧键
    // 仍得正确的开/关语义（仅"Shift 或 Ctrl"细模式对旧版退化为"开"）
    SetDword(hKey, L"ShiftToggle", s.toggleMode != 0 ? 1 : 0);
    SetDword(hKey, L"ToggleMode", s.toggleMode);
    SetDword(hKey, L"Shuangpin", s.shuangpin ? 1 : 0);
    SetDword(hKey, L"ShuangpinScheme", s.shuangpinScheme);
    SetDword(hKey, L"SymbolHotkey", s.symbolHotkey ? 1 : 0);
    SetDword(hKey, L"Traditional", s.traditionalOutput ? 1 : 0);
    SetDword(hKey, L"LlmRerank", s.llmRerank ? 1 : 0);
    SetDword(hKey, L"LlmMultiPath", s.llmMultiPath ? 1 : 0);
    SetDword(hKey, L"SkinIndex", s.skinIndex);
    SetDword(hKey, L"UiFont", s.uiFont);
    SetDword(hKey, L"CandidateOpacity", s.candidateOpacity);
    SetDword(hKey, L"AssocAfterPick", s.assocAfterPick ? 1u : 0u);
    SetDword(hKey, L"CandidatePinned", s.candidatePinned);
    SetDword(hKey, L"CandidatePinX", (DWORD)(uint32_t)s.candidatePinX);
    SetDword(hKey, L"CandidatePinY", (DWORD)(uint32_t)s.candidatePinY);
    SetString(hKey, L"CellDisabled", s.cellDisabled);
    SetString(hKey, L"RecentSymbols", s.recentSymbols);
    SetString(hKey, L"FavoriteSymbols", s.favoriteSymbols);
    SetDword(hKey, L"DailyCharGoal", s.dailyCharGoal);
    SetDword(hKey, L"LastSettingsPage", s.lastSettingsPage);
    SetDword(hKey, L"LastSymbolCat", s.lastSymbolCat);
    SetDword(hKey, L"SetWinX", (DWORD)(uint32_t)s.setWinX);
    SetDword(hKey, L"SetWinY", (DWORD)(uint32_t)s.setWinY);
    SetDword(hKey, L"SetWinW", (DWORD)(uint32_t)s.setWinW);
    SetDword(hKey, L"SetWinH", (DWORD)(uint32_t)s.setWinH);
    SetString(hKey, L"UpdateRepo", s.updateRepo);
    SetDword(hKey, L"UpdateAutoCheck", s.updateAutoCheck);
    SetQword(hKey, L"UpdateLastCheck", s.updateLastCheck);

    BumpSettingsSeq(hKey); // M5：版本 bump，读侧缓存即时失效
    RegCloseKey(hKey);
}


namespace {
// 单键 diff 写：值与当前磁盘值不同才写（写失败静默，语义同 SaveSettings）
void PutDwordDiff(HKEY hKey, const wchar_t* name, DWORD want)
{
    DWORD cur = 0;
    if (GetDword(hKey, name, cur) && cur == want) return;
    SetDword(hKey, name, want);
}

void PutStringDiff(HKEY hKey, const wchar_t* name, const std::wstring& want)
{
    std::wstring cur;
    if (GetString(hKey, name, cur) && cur == want) return;
    SetString(hKey, name, want);
}
} // namespace

void SaveSettingsDiff(const Settings& s)
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ActiveKey(), 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_READ | KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return;
    PutDwordDiff(hKey, L"PinyinEnabled", s.pinyinEnabled ? 1 : 0);
    PutDwordDiff(hKey, L"CandidateCount", s.candidateCount);
    PutDwordDiff(hKey, L"CandPageSize", s.candPageSize);
    PutDwordDiff(hKey, L"CandidateFontSize", s.candidateFontSize);
    PutDwordDiff(hKey, L"CandOrientation", s.candOrientation);
    PutDwordDiff(hKey, L"DecayEnabled", s.decayEnabled);
    PutDwordDiff(hKey, L"HalfLifeDays", s.decayHalfLifeDays);
    PutDwordDiff(hKey, L"DarkMode", s.darkMode);
    PutDwordDiff(hKey, L"EnCompletion", s.enCompletion ? 1 : 0);
    PutDwordDiff(hKey, L"Paused", s.paused ? 1 : 0);
    PutDwordDiff(hKey, L"Incognito", s.incognito ? 1 : 0);
    PutDwordDiff(hKey, L"CnPunct", s.cnPunct ? 1 : 0);
    PutDwordDiff(hKey, L"FullWidthAlnum", s.fullWidthAlnum ? 1 : 0);
    PutDwordDiff(hKey, L"FuzzyPinyin", s.fuzzyPinyin ? 1 : 0);
    PutDwordDiff(hKey, L"FuzzyGroups", s.fuzzyGroups);
    PutDwordDiff(hKey, L"ShiftToggle", s.toggleMode != 0 ? 1 : 0);
    PutDwordDiff(hKey, L"ToggleMode", s.toggleMode);
    PutDwordDiff(hKey, L"Shuangpin", s.shuangpin ? 1 : 0);
    PutDwordDiff(hKey, L"ShuangpinScheme", s.shuangpinScheme);
    PutDwordDiff(hKey, L"SymbolHotkey", s.symbolHotkey ? 1 : 0);
    PutDwordDiff(hKey, L"Traditional", s.traditionalOutput ? 1 : 0);
    PutDwordDiff(hKey, L"LlmRerank", s.llmRerank ? 1 : 0);
    PutDwordDiff(hKey, L"LlmMultiPath", s.llmMultiPath ? 1 : 0);
    PutDwordDiff(hKey, L"SkinIndex", s.skinIndex);
    PutDwordDiff(hKey, L"UiFont", s.uiFont);
    PutDwordDiff(hKey, L"CandidateOpacity", s.candidateOpacity);
    PutDwordDiff(hKey, L"AssocAfterPick", s.assocAfterPick ? 1u : 0u);
    PutDwordDiff(hKey, L"CandidatePinned", s.candidatePinned);
    PutDwordDiff(hKey, L"CandidatePinX", (DWORD)(uint32_t)s.candidatePinX);
    PutDwordDiff(hKey, L"CandidatePinY", (DWORD)(uint32_t)s.candidatePinY);
    PutStringDiff(hKey, L"CellDisabled", s.cellDisabled);
    PutStringDiff(hKey, L"RecentSymbols", s.recentSymbols);
    PutStringDiff(hKey, L"FavoriteSymbols", s.favoriteSymbols);
    PutDwordDiff(hKey, L"DailyCharGoal", s.dailyCharGoal);
    PutDwordDiff(hKey, L"LastSettingsPage", s.lastSettingsPage);
    PutDwordDiff(hKey, L"LastSymbolCat", s.lastSymbolCat);
    PutDwordDiff(hKey, L"SetWinX", (DWORD)(uint32_t)s.setWinX);
    PutDwordDiff(hKey, L"SetWinY", (DWORD)(uint32_t)s.setWinY);
    PutDwordDiff(hKey, L"SetWinW", (DWORD)(uint32_t)s.setWinW);
    PutDwordDiff(hKey, L"SetWinH", (DWORD)(uint32_t)s.setWinH);
    PutStringDiff(hKey, L"UpdateRepo", s.updateRepo);
    PutDwordDiff(hKey, L"UpdateAutoCheck", s.updateAutoCheck);
    SetQword(hKey, L"UpdateLastCheck", s.updateLastCheck);
    BumpSettingsSeq(hKey); // M5：版本 bump，读侧缓存即时失效
    RegCloseKey(hKey);
}

void ResetSettingsToDefaults()
{
    // RegDeleteTreeW 连键删除（比逐值枚举删除简单且原子）；下次保存时
    // RegCreateKeyExW 自动重建，LoadSettings 走"键不存在=全默认"路径。
    RegDeleteTreeW(HKEY_CURRENT_USER, ActiveKey());
}
} // namespace lingxi
