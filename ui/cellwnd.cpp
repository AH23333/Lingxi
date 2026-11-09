// cellwnd.cpp - A 细胞词库管理窗（design-cell-dict-2026-11-03 §7 A5；新功能新文件）
//
// 职责：列表（扫 %APPDATA%\LingxiIme\cell\*.lxcell 头+行数）、安装（文件选择→
// 后台线程 ImportCellPackSync→PostMessage 回主线程报"成功N跳过M(行号)"，禁静默
// 丢弃）、启用/停用（settings Cell.disabled 增删 id——引擎在预测线程按变更自动
// 重建，UI 无需显式通知）、删除（删文件 + 剔停用表 + reloadCellPacks 重扫）。
// 红线：本窗绝不触碰引擎 _entries/索引；导入只在后台线程碰磁盘与内存副本。
#include "uiinternal.h"
#include "cellimport.h"
#include "mgmt_logic.h" // 批14：过滤/停用表/批量启停纯逻辑（engine_test MG5..MG8 直链单测）
#include "engine/predictor.h"

#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <string>
#include <vector>

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kCellWndClass[] = L"LingxiCellWnd";
constexpr UINT WM_APP_CELL_DONE = WM_APP + 61; // lParam=CellImportResult*（处理方 delete）

enum
{
    IDC_CELL_STATIC = 5100,
    IDC_CELL_LIST,
    IDC_CELL_INSTALL,
    IDC_CELL_ENABLE,
    IDC_CELL_DISABLE,
    IDC_CELL_DELETE,
    IDC_CELL_CLOSE,
    IDC_CELL_SEARCH, // 批14：搜索过滤框（包名/id 子串实时过滤）
    IDC_CELL_URLEDIT,  // 在线源：包 URL（https，.lxcell）
    IDC_CELL_SHAEDIT,  // 在线源：SHA256（可空=侧车）
    IDC_CELL_URLFETCH, // 在线源：从 URL 导入按钮
};

HWND g_hCellWnd = NULL;
HWND g_hCellSearch = NULL; // 批14 搜索框
bool g_cellImporting = false; // 安装进行中防重入
std::wstring g_lastImportSrc; // "已存在→确认覆盖"重跑用（源路径在结果回调时已不可得）
bool g_lastImportIsUrl = false;  // 在线源重跑标记（覆盖重跑须重新下载而非复读旧件）
std::wstring g_lastImportSha;    // 在线源 SHA256（可空=侧车）
HWND g_hCellUrlEdit = NULL, g_hCellShaEdit = NULL;

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

struct CellRow { std::wstring id, name; size_t entries = 0; bool disabled = false; };

// 轻量列表：只读头（name=/id=）+ 统计含 Tab 的词条行数，不做音节校验（UI 线程，
// 包文件 ≤ 数 MB，秒级可接受；完整校验在导入线程已做过）
std::vector<CellRow> ScanPacks()
{
    std::vector<CellRow> rows;
    std::wstring dir = CellPackDirW();
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.lxcell").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return rows;
    std::vector<std::wstring> files;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    Settings st = LoadSettings();
    std::vector<std::wstring> dis = mgmt::SplitIds(st.cellDisabled);
    for (const auto& f : files)
    {
        std::string raw;
        if (!ReadFileBytes(dir + L"\\" + f, raw)) continue;
        CellRow row;
        bool inHeader = true;
        size_t pos = 0;
        while (pos <= raw.size())
        {
            size_t nl = raw.find('\n', pos);
            std::string line = (nl == std::string::npos) ? raw.substr(pos)
                                                         : raw.substr(pos, nl - pos);
            pos = (nl == std::string::npos) ? raw.size() + 1 : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == ';') continue;
            if (line.rfind("LXCELL", 0) == 0) continue;
            if (inHeader && line.find('\t') == std::string::npos &&
                (line.rfind("name=", 0) == 0 || line.rfind("id=", 0) == 0 ||
                 line.rfind("weight=", 0) == 0))
            {
                if (line.rfind("name=", 0) == 0) row.name = Utf8ToW(line.substr(5));
                else if (line.rfind("id=", 0) == 0) row.id = Utf8ToW(line.substr(3));
                continue;
            }
            inHeader = false;
            if (line.find('\t') != std::string::npos) row.entries++;
        }
        if (row.id.empty()) row.id = f.substr(0, f.size() - 7); // 去 .lxcell 兜底
        if (row.name.empty()) row.name = row.id;
        for (const auto& d : dis) if (d == row.id) { row.disabled = true; break; }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const CellRow& a, const CellRow& b) { return a.id < b.id; });
    return rows;
}

// 批14 多选：选择集（LB_GETSEL）→ id 列表（行格式 "id | name | N 条 | 状态"，
// 取首个 " |" 前的 id；解析不出的行跳过）
std::vector<std::wstring> SelectedRowIds()
{
    std::vector<std::wstring> ids;
    HWND lb = GetDlgItem(g_hCellWnd, IDC_CELL_LIST);
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++)
    {
        if ((int)SendMessageW(lb, LB_GETSEL, i, 0) <= 0) continue;
        wchar_t line[512] = L"";
        SendMessageW(lb, LB_GETTEXT, i, (LPARAM)line);
        const wchar_t* sep = wcsstr(line, L" | ");
        if (sep) ids.push_back(std::wstring(line, sep - line));
    }
    return ids;
}

void RefreshCellList()
{
    HWND lb = GetDlgItem(g_hCellWnd, IDC_CELL_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    // 批14 搜索过滤：id 或包名子串（不区分大小写）；判定在 mgmt::CellFilterMatch
    //（engine_test MG7 钉口径）；空 filter=全量。
    std::wstring flt;
    if (g_hCellSearch)
    {
        wchar_t buf[128] = L"";
        GetWindowTextW(g_hCellSearch, buf, 128);
        flt = buf;
    }
    std::vector<CellRow> rows = ScanPacks();
    size_t shown = 0;
    for (const auto& r : rows)
    {
        if (!mgmt::CellFilterMatch(r.id, r.name, flt)) continue;
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%s | %s | %llu 条 | %s", r.id.c_str(),
                     r.name.c_str(), (unsigned long long)r.entries,
                     r.disabled ? L"已停用" : L"启用中");
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
        shown++;
    }
    if (rows.empty())
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"（未安装任何词库包——点「安装包」导入 .lxcell 或深蓝纯文本产物）");
    else if (shown == 0)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"（无匹配词库包，清空搜索框恢复全量）");
    if (!flt.empty())
    {
        wchar_t cap[96];
        _snwprintf_s(cap, _TRUNCATE, L"匹配 %zu / %zu 包（清空搜索框恢复全量）",
                     shown, rows.size());
        SetDlgItemTextW(g_hCellWnd, IDC_CELL_STATIC, cap);
    }
}

struct CellImportJob { HWND hwnd; std::wstring src; bool overwrite; bool isUrl; std::wstring sha; };

// N9：导入结果所有权槽（至多一个在途——g_cellImporting 防重入保证）。
// 投递前登记，窗口处理/销毁领取；投递失败仅当领回自己的指针才回收。
mgmt::PendingSlot<CellImportResult> g_cellResSlot;

DWORD WINAPI CellImportThread(LPVOID p)
{
    CellImportJob* job = (CellImportJob*)p;
    std::vector<std::string> syls;
    if (g_hooks.getSyllables) g_hooks.getSyllables(syls);
    std::unordered_set<std::string> set(syls.begin(), syls.end());
    CellImportResult* res = new CellImportResult(
        job->isUrl ? ImportCellPackFromUrl(job->src, job->sha, set, job->overwrite)
                   : ImportCellPackSync(job->src, set, job->overwrite));
    if (!IsWindow(job->hwnd))
    {
        delete res; // 窗已灭：直接释放（导入件本身已落盘，下次开窗可见）
        delete job;
        return 0;
    }
    // N9：先登记再投递——窗口在消息处理前销毁时由 WM_DESTROY 领取回收，
    // 结果不再悬在队列里随窗泄漏
    if (CellImportResult* stale = g_cellResSlot.Register(res)) delete stale; // 防御（防重入已在）
    if (!PostMessageW(job->hwnd, WM_APP_CELL_DONE, res->ok ? 1 : 0, (LPARAM)res))
    {
        if (g_cellResSlot.Claim() == res) delete res; // 未被窗口侧领取才回收
    }
    delete job;
    return 0;
}

void StartImport(HWND hwnd, const std::wstring& src, bool overwrite)
{
    g_cellImporting = true;
    g_lastImportSrc = src;
    g_lastImportIsUrl = false;
    g_lastImportSha.clear();
    EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CELL_URLFETCH), FALSE);
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC,
                    L"正在后台导入（不阻塞输入与界面）……完成后此处报告结果");
    CellImportJob* job = new CellImportJob{ hwnd, src, overwrite, false, L"" };
    HANDLE h = CreateThread(NULL, 0, CellImportThread, job, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        g_cellImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_URLFETCH), TRUE);
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"无法创建导入线程");
    }
}

// 在线源导入：下载→SHA256 锚校验→与本地同管安装（后台线程，模型一致）
void StartUrlImport(HWND hwnd, const std::wstring& url, const std::wstring& sha, bool overwrite)
{
    g_cellImporting = true;
    g_lastImportSrc = url;
    g_lastImportIsUrl = true;
    g_lastImportSha = sha;
    EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_CELL_URLFETCH), FALSE);
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC,
                    L"正在后台下载并校验（不阻塞输入与界面）……完成后此处报告结果");
    CellImportJob* job = new CellImportJob{ hwnd, url, overwrite, true, sha };
    HANDLE h = CreateThread(NULL, 0, CellImportThread, job, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        g_cellImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_URLFETCH), TRUE);
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"无法创建导入线程");
    }
}

void DoInstall(HWND hwnd)
{
    if (g_cellImporting) return;
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{ sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"词库包 (*.lxcell)\0*.lxcell\0纯文本词库 (*.txt)\0*.txt\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    StartImport(hwnd, file, false);
}

void DoUrlFetch(HWND hwnd)
{
    if (g_cellImporting) return;
    wchar_t url[2048] = L"", sha[80] = L"";
    GetWindowTextW(g_hCellUrlEdit, url, 2047);
    GetWindowTextW(g_hCellShaEdit, sha, 79);
    auto trim = [](wchar_t* s) {
        wchar_t* b = s;
        while (*b == L' ' || *b == L'\t') b++;
        wchar_t* e = b + wcslen(b);
        while (e > b && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' || e[-1] == L'\n')) --e;
        *e = 0;
        if (b != s) memmove(s, b, (e - b + 1) * sizeof(wchar_t));
    };
    trim(url);
    trim(sha);
    if (!url[0])
    {
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC,
                        L"请先粘贴词库包 URL（https，.lxcell 结尾；出网域名白名单为 GitHub）");
        return;
    }
    StartUrlImport(hwnd, url, sha, false);
}

void DoToggle(HWND hwnd, bool dis)
{
    // 批14 批量启停：选择集 id 列表 → mgmt::ApplyBulkDisabled 一次算出新停用表
    // → 单次 SaveSettingsDiff 整批落盘（engine_test MG5/MG8 钉表计算口径）
    std::vector<std::wstring> ids = SelectedRowIds();
    if (ids.empty()) { SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"请先在列表选中词库包（可 Ctrl/Shift 多选）"); return; }
    Settings st = LoadSettings();
    st.cellDisabled = mgmt::ApplyBulkDisabled(st.cellDisabled, ids, dis);
    // N6：diff 落盘——只写本窗负责的 cellDisabled，不再全量回写覆盖设置页
    // 的并发改动（机制钉：engine_test "N6 full-save clobbers..."）
    SaveSettingsDiff(st); // 引擎在下一次 Predict 检出变更并重建（无锁红线）
    RefreshCellList();
    wchar_t buf[256];
    _snwprintf_s(buf, _TRUNCATE, L"已%s %d 个包：下一次输入即%s", dis ? L"停用" : L"启用",
                 (int)ids.size(), dis ? L"消失" : L"出现");
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC, buf);
}

void DoDelete(HWND hwnd)
{
    // 批14 批量删除：逐包删文件（占用→rename-aside 让位，#89），成功项整批
    // 剔停用表 + 单次 reload；失败项明示不静默
    std::vector<std::wstring> ids = SelectedRowIds();
    if (ids.empty()) { SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"请先在列表选中词库包（可 Ctrl/Shift 多选）"); return; }
    std::wstring q;
    if (ids.size() == 1)
        q = L"删除词库包 " + ids[0] + L"？（不影响用户词典与自学习数据）";
    else
        q = L"删除选中的 " + std::to_wstring(ids.size()) + L" 个词库包？（不影响用户词典与自学习数据）";
    if (MessageBoxW(hwnd, q.c_str(), L"确认删除", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    std::vector<std::wstring> deleted, failed;
    for (const auto& id : ids)
    {
        std::wstring path = CellPackDirW() + L"\\" + id + L".lxcell";
        if (DeleteFileW(path.c_str())) { deleted.push_back(id); continue; }
        // 被占用（预测线程正在读）：rename-aside 等效删除（#89）
        std::wstring aside = path + L"." + std::to_wstring(GetTickCount()) + L".del";
        if (MoveFileExW(path.c_str(), aside.c_str(), 0)) deleted.push_back(id);
        else failed.push_back(id);
    }
    // N7：rename-aside 产出的 .del 立即试清（引擎句柄释放后即可删净）
    {
        size_t cleanFailed = 0;
        size_t cleaned = CleanupCellDelResidue(&cleanFailed);
        if (cleaned || cleanFailed)
            LX_LOG(L"ui", L"删除后 .del 清理: 删 %zu 失败 %zu", cleaned, cleanFailed);
    }
    if (!deleted.empty())
    {
        Settings st = LoadSettings();
        st.cellDisabled = mgmt::ApplyBulkDisabled(st.cellDisabled, deleted, false); // 剔停用表残留项
        SaveSettingsDiff(st); // N6：diff 落盘，不覆盖设置页并发改动
        if (g_hooks.reloadCellPacks) g_hooks.reloadCellPacks();
    }
    RefreshCellList();
    if (deleted.empty())
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"删除失败：文件被占用且改名让位未成");
    else
    {
        wchar_t buf[256];
        _snwprintf_s(buf, _TRUNCATE, L"已删除 %d 个词库包%s", (int)deleted.size(),
                     failed.empty() ? L"" : L"（部分文件被占用未删）");
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, buf);
    }
}

LRESULT CALLBACK CellWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CELL_INSTALL: DoInstall(hwnd); return 0;
        case IDC_CELL_URLFETCH: DoUrlFetch(hwnd); return 0;
        case IDC_CELL_ENABLE: DoToggle(hwnd, false); return 0;
        case IDC_CELL_DISABLE: DoToggle(hwnd, true); return 0;
        case IDC_CELL_DELETE: DoDelete(hwnd); return 0;
        case IDC_CELL_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        case IDC_CELL_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE) RefreshCellList(); // 实时过滤
            return 0;
        }
        break;
    case WM_APP_CELL_DONE:
    {
        // N9：只信槽——槽空=所有权已被领取（如 WM_DESTROY 先收），
        // lParam 指针此时不可再用（悬垂），直接忽略
        CellImportResult* res = g_cellResSlot.Claim();
        if (!res) return 0;
        g_cellImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_URLFETCH), TRUE);
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, res->message.c_str());
        if (res->ok && g_hooks.reloadCellPacks) g_hooks.reloadCellPacks();
        else if (!res->ok && res->message.find(L"已存在") != std::wstring::npos)
        {
            // 同 id 已存在：询问覆盖后用记住的源路径重跑（rename-aside，失败自动回滚）
            int ans = MessageBoxW(hwnd,
                (std::wstring(L"该包 id 已存在：") + Utf8ToW(res->id) +
                 L"\n\n用新文件覆盖旧包？").c_str(),
                L"覆盖确认", MB_YESNO | MB_ICONQUESTION);
            if (ans == IDYES) // MB_YESNO 只回 IDYES/IDNO——判 IDOK 恒假，"是"永远无法覆盖
            {
                RefreshCellList();
                delete res;
                // 在线源覆盖=重新下载校验（不复读旧 staging，防"已确认件被调包"）
                if (g_lastImportIsUrl) StartUrlImport(hwnd, g_lastImportSrc, g_lastImportSha, true);
                else                   StartImport(hwnd, g_lastImportSrc, true);
                return 0;
            }
        }
        RefreshCellList();
        delete res;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)g_hBgBrush;
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        // N9：窗口销毁先于 WM_APP_CELL_DONE 处理时，队列消息随窗丢弃——
        // 结果在槽里，此处领取回收（无在途导入则 Claim 得 nullptr，无操作）
        if (CellImportResult* res = g_cellResSlot.Claim()) delete res;
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowCellDictWindow(HINSTANCE hInst)
{
    // N7：开窗即清 rename-aside 残留的 .del 死件（占用中删不掉则记日志，下次再试）
    {
        size_t failed = 0;
        size_t n = CleanupCellDelResidue(&failed);
        if (n || failed)
            LX_LOG(L"ui", L"细胞词库 .del 残留清理: 删 %zu 失败 %zu", n, failed);
    }
    if (g_hCellWnd)
    {
        ShowWindow(g_hCellWnd, SW_SHOW);
        SetForegroundWindow(g_hCellWnd);
        RefreshCellList();
        return;
    }
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = CellWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!g_hBgBrush) g_hBgBrush = CreateSolidBrush(kUiBg);
    wc.hbrBackground = g_hBgBrush;
    wc.lpszClassName = kCellWndClass;
    RegisterClassExW(&wc);

    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int W = MulDiv(560, dpi, 96), H = MulDiv(420, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int pad = MulDiv(12, dpi, 96);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int winW = W, winH = H;
    {
        RECT rc{ 0, 0, winW, winH };
        if (AdjustWindowRectEx(&rc, style, FALSE, 0))
        { winW = rc.right - rc.left; winH = rc.bottom - rc.top; }
    }
    g_hCellWnd = CreateWindowExW(0, kCellWndClass, L"词库管理（细胞词库）", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, NULL, NULL, hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                    pad, pad, W - pad * 2, btnH, g_hCellWnd,
                    (HMENU)(INT_PTR)IDC_CELL_STATIC, hInst, NULL);

    // 批14 搜索过滤行（包名/id 子串实时过滤；清空恢复全量）
    g_hCellSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    pad, pad + btnH + 4, W - pad * 2, btnH, g_hCellWnd,
                                    (HMENU)(INT_PTR)IDC_CELL_SEARCH, hInst, NULL);
    SendMessageW(g_hCellSearch, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"搜索：包名或 id（实时过滤）");

    int rowBtnY = H - btnH - pad;
    int listTop = pad + btnH * 2 + 10;
    // 在线源行（按钮行上方）：URL + SHA256（可空=侧车）+ 导入按钮
    int urlRowY = rowBtnY - btnH - 6;
    int urlW = (W - pad * 2) * 52 / 100;
    int shaW = (W - pad * 2) * 30 / 100;
    int fetchW = W - pad * 2 - urlW - shaW - 10;
    g_hCellUrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     pad, urlRowY, urlW, btnH, g_hCellWnd,
                                     (HMENU)(INT_PTR)IDC_CELL_URLEDIT, hInst, NULL);
    SendMessageW(g_hCellUrlEdit, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"在线包 URL（https://…/*.lxcell，GitHub 白名单）");
    g_hCellShaEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     pad + urlW + 5, urlRowY, shaW, btnH, g_hCellWnd,
                                     (HMENU)(INT_PTR)IDC_CELL_SHAEDIT, hInst, NULL);
    SendMessageW(g_hCellShaEdit, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"SHA256（可空=自动取侧车 .sha256）");
    CreateWindowExW(0, L"BUTTON", L"URL 导入",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    pad + urlW + shaW + 10, urlRowY, fetchW, btnH, g_hCellWnd,
                    (HMENU)(INT_PTR)IDC_CELL_URLFETCH, hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                    LBS_EXTENDEDSEL, // 批14：批量启停/删除（Ctrl/Shift 点选）
                    pad, listTop, W - pad * 2, urlRowY - listTop - 6, g_hCellWnd,
                    (HMENU)(INT_PTR)IDC_CELL_LIST, hInst, NULL);

    struct Btn { const wchar_t* text; int id; };
    Btn btns[] = {
        { L"安装包", IDC_CELL_INSTALL }, { L"启用", IDC_CELL_ENABLE },
        { L"停用", IDC_CELL_DISABLE }, { L"删除", IDC_CELL_DELETE },
        { L"关闭", IDC_CELL_CLOSE },
    };
    int bw = (W - pad * 2 - 4 * 5) / 5;
    for (int i = 0; i < 5; i++)
        CreateWindowExW(0, L"BUTTON", btns[i].text,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        pad + i * (bw + 5), rowBtnY, bw, btnH, g_hCellWnd,
                        (HMENU)(INT_PTR)btns[i].id, hInst, NULL);

    if (g_hFont)
    {
        SendMessageW(g_hCellWnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (int id : { IDC_CELL_STATIC, IDC_CELL_LIST, IDC_CELL_SEARCH, IDC_CELL_URLEDIT,
                        IDC_CELL_SHAEDIT, IDC_CELL_URLFETCH, IDC_CELL_INSTALL, IDC_CELL_ENABLE,
                        IDC_CELL_DISABLE, IDC_CELL_DELETE, IDC_CELL_CLOSE })
            SendMessageW(GetDlgItem(g_hCellWnd, id), WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }
    RefreshCellList();
    ShowWindow(g_hCellWnd, SW_SHOW);
}

} // namespace lingxi
