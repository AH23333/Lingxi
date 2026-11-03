// cellwnd.cpp - A 细胞词库管理窗（design-cell-dict-2026-11-03 §7 A5；新功能新文件）
//
// 职责：列表（扫 %APPDATA%\LingxiIme\cell\*.lxcell 头+行数）、安装（文件选择→
// 后台线程 ImportCellPackSync→PostMessage 回主线程报"成功N跳过M(行号)"，禁静默
// 丢弃）、启用/停用（settings Cell.disabled 增删 id——引擎在预测线程按变更自动
// 重建，UI 无需显式通知）、删除（删文件 + 剔停用表 + reloadCellPacks 重扫）。
// 红线：本窗绝不触碰引擎 _entries/索引；导入只在后台线程碰磁盘与内存副本。
#include "uiinternal.h"
#include "cellimport.h"
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
};

HWND g_hCellWnd = NULL;
bool g_cellImporting = false; // 安装进行中防重入
std::wstring g_lastImportSrc; // "已存在→确认覆盖"重跑用（源路径在结果回调时已不可得）

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 停用表读写（settings cellDisabled：逗号分隔 id）
std::vector<std::wstring> DisabledIds(const std::wstring& raw)
{
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= raw.size())
    {
        size_t c = raw.find(L',', pos);
        std::wstring seg = raw.substr(pos, c == std::wstring::npos ? c : c - pos);
        while (!seg.empty() && (seg.front() == L' ' || seg.front() == L'\t')) seg.erase(seg.begin());
        while (!seg.empty() && (seg.back() == L' ' || seg.back() == L'\t')) seg.pop_back();
        if (!seg.empty()) out.push_back(seg);
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
    return out;
}

void SetPackDisabled(const std::wstring& id, bool dis)
{
    Settings st = LoadSettings();
    std::vector<std::wstring> cur = DisabledIds(st.cellDisabled);
    bool has = false;
    for (size_t i = 0; i < cur.size(); i++)
        if (cur[i] == id) { cur.erase(cur.begin() + i); has = true; break; }
    if (dis && !has) cur.push_back(id);
    std::wstring joined;
    for (size_t i = 0; i < cur.size(); i++) { if (i) joined += L','; joined += cur[i]; }
    st.cellDisabled = joined;
    SaveSettings(st); // 引擎在下一次 Predict 检出变更并重建（无锁红线）
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
    std::vector<std::wstring> dis = DisabledIds(st.cellDisabled);
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

std::wstring SelectedRowId()
{
    int sel = (int)SendMessageW(GetDlgItem(g_hCellWnd, IDC_CELL_LIST), LB_GETCURSEL, 0, 0);
    if (sel < 0) return {};
    wchar_t line[512] = L"";
    SendMessageW(GetDlgItem(g_hCellWnd, IDC_CELL_LIST), LB_GETTEXT, sel, (LPARAM)line);
    // 行格式 "id | name | N 条 | 状态"：取首个 " |" 前的 id
    const wchar_t* sep = wcsstr(line, L" | ");
    return sep ? std::wstring(line, sep - line) : std::wstring();
}

void RefreshCellList()
{
    HWND lb = GetDlgItem(g_hCellWnd, IDC_CELL_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    std::vector<CellRow> rows = ScanPacks();
    for (const auto& r : rows)
    {
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%s | %s | %llu 条 | %s", r.id.c_str(),
                     r.name.c_str(), (unsigned long long)r.entries,
                     r.disabled ? L"已停用" : L"启用中");
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (rows.empty())
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"（未安装任何词库包——点「安装包」导入 .lxcell 或深蓝纯文本产物）");
}

struct CellImportJob { HWND hwnd; std::wstring src; bool overwrite; };

DWORD WINAPI CellImportThread(LPVOID p)
{
    CellImportJob* job = (CellImportJob*)p;
    std::vector<std::string> syls;
    if (g_hooks.getSyllables) g_hooks.getSyllables(syls);
    std::unordered_set<std::string> set(syls.begin(), syls.end());
    CellImportResult* res = new CellImportResult(
        ImportCellPackSync(job->src, set, job->overwrite));
    if (!IsWindow(job->hwnd) ||
        !PostMessageW(job->hwnd, WM_APP_CELL_DONE, res->ok ? 1 : 0, (LPARAM)res))
        delete res; // 窗已灭：直接释放（导入件本身已落盘，下次开窗可见）
    delete job;
    return 0;
}

void StartImport(HWND hwnd, const std::wstring& src, bool overwrite)
{
    g_cellImporting = true;
    g_lastImportSrc = src;
    EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), FALSE);
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC,
                    L"正在后台导入（不阻塞输入与界面）……完成后此处报告结果");
    CellImportJob* job = new CellImportJob{ hwnd, src, overwrite };
    HANDLE h = CreateThread(NULL, 0, CellImportThread, job, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        g_cellImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), TRUE);
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

void DoToggle(HWND hwnd, bool dis)
{
    std::wstring id = SelectedRowId();
    if (id.empty()) { SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"请先在列表选中一个词库包"); return; }
    SetPackDisabled(id, dis);
    RefreshCellList();
    wchar_t buf[256];
    _snwprintf_s(buf, _TRUNCATE, L"%s %s：下一次输入即%s", id.c_str(),
                 dis ? L"已停用" : L"已启用", dis ? L"消失" : L"出现");
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC, buf);
}

void DoDelete(HWND hwnd)
{
    std::wstring id = SelectedRowId();
    if (id.empty()) { SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"请先在列表选中一个词库包"); return; }
    if (MessageBoxW(hwnd, (L"删除词库包 " + id + L"？（不影响用户词典与自学习数据）").c_str(),
                    L"确认删除", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    std::wstring path = CellPackDirW() + L"\\" + id + L".lxcell";
    if (!DeleteFileW(path.c_str()))
    {
        // 被占用（预测线程正在读）：rename-aside 等效删除（#89）
        std::wstring aside = path + L"." + std::to_wstring(GetTickCount()) + L".del";
        if (!MoveFileExW(path.c_str(), aside.c_str(), 0))
        {
            SetDlgItemTextW(hwnd, IDC_CELL_STATIC, L"删除失败：文件被占用且改名让位未成");
            return;
        }
    }
    SetPackDisabled(id, false); // 剔停用表残留项
    if (g_hooks.reloadCellPacks) g_hooks.reloadCellPacks();
    RefreshCellList();
    SetDlgItemTextW(hwnd, IDC_CELL_STATIC, (L"已删除词库包 " + id).c_str());
}

LRESULT CALLBACK CellWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CELL_INSTALL: DoInstall(hwnd); return 0;
        case IDC_CELL_ENABLE: DoToggle(hwnd, false); return 0;
        case IDC_CELL_DISABLE: DoToggle(hwnd, true); return 0;
        case IDC_CELL_DELETE: DoDelete(hwnd); return 0;
        case IDC_CELL_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_APP_CELL_DONE:
    {
        CellImportResult* res = (CellImportResult*)lParam;
        g_cellImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_CELL_INSTALL), TRUE);
        SetDlgItemTextW(hwnd, IDC_CELL_STATIC, res->message.c_str());
        if (res->ok && g_hooks.reloadCellPacks) g_hooks.reloadCellPacks();
        else if (!res->ok && res->message.find(L"已存在") != std::wstring::npos)
        {
            // 同 id 已存在：询问覆盖后用记住的源路径重跑（rename-aside，失败自动回滚）
            int ans = MessageBoxW(hwnd,
                (std::wstring(L"该包 id 已存在：") + Utf8ToW(res->id) +
                 L"\n\n用新文件覆盖旧包？").c_str(),
                L"覆盖确认", MB_YESNO | MB_ICONQUESTION);
            if (ans == IDOK)
            {
                RefreshCellList();
                delete res;
                StartImport(hwnd, g_lastImportSrc, true);
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
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowCellDictWindow(HINSTANCE hInst)
{
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

    int rowBtnY = H - btnH - pad;
    int listTop = pad + btnH + 6;
    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                    pad, listTop, W - pad * 2, rowBtnY - listTop - 8, g_hCellWnd,
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
        for (int id : { IDC_CELL_STATIC, IDC_CELL_LIST, IDC_CELL_INSTALL, IDC_CELL_ENABLE,
                        IDC_CELL_DISABLE, IDC_CELL_DELETE, IDC_CELL_CLOSE })
            SendMessageW(GetDlgItem(g_hCellWnd, id), WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }
    RefreshCellList();
    ShowWindow(g_hCellWnd, SW_SHOW);
}

} // namespace lingxi
