// skinwnd.cpp - 在线皮肤管理窗（新功能新文件，settingsui 只减不增纪律；托盘入口）
//
// 职责：列表（%APPDATA%\LingxiIme\skins\*.lxskin，ScanSkinDir 同口径）、
// URL 导入（后台线程 ImportSkinFromUrl——https 形闸+sha256 锚，不阻塞 UI）、
// 应用（写 settings.skinId——渲染侧 UiCurrentSkin 下次 Show/Paint 即时生效，
// 无需通知引擎）、删除（文件删除+若正引用则清 skinId——UiCurrentSkin 本就会
// 回退内置，显式清是为免残留垃圾 id）、用回内置（清 skinId）。
// 红线：本窗绝不触碰渲染状态；换肤只经注册表设置 + UiReloadSkins 重扫。
#include "uiinternal.h"
#include "skinstore.h"
#include "skinformat_logic.h" // ValidateSkinUrl（提示文案前置校验，不必等线程）
#include "mgmt_logic.h"       // PendingSlot（导入结果所有权槽，N9 同型）

#include <windows.h>
#include <string>
#include <vector>

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kSkinWndClass[] = L"LingxiSkinWnd";
constexpr UINT WM_APP_SKIN_DONE = WM_APP + 62; // lParam=SkinImportResult*（处理方 delete）

enum
{
    IDC_SKIN_STATIC = 5200,
    IDC_SKIN_LIST,
    IDC_SKIN_APPLY,
    IDC_SKIN_USEBUILTIN,
    IDC_SKIN_DELETE,
    IDC_SKIN_CLOSE,
    IDC_SKIN_URLEDIT,
    IDC_SKIN_SHAEDIT,
    IDC_SKIN_URLFETCH,
};

HWND g_hSkinWnd = NULL;
HWND g_hSkinUrlEdit = NULL, g_hSkinShaEdit = NULL;
bool g_skinImporting = false; // 导入进行中防重入

mgmt::PendingSlot<SkinImportResult> g_skinResSlot;

void RefreshSkinList()
{
    HWND lb = GetDlgItem(g_hSkinWnd, IDC_SKIN_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    size_t skipped = 0;
    std::vector<DlSkin> rows;
    ScanSkinDir(SkinDirW(), &rows, &skipped); // 直接扫盘（含未入注册表的新件）
    Settings st = LoadSettings();
    for (const auto& r : rows)
    {
        wchar_t buf[512];
        _snwprintf_s(buf, _TRUNCATE, L"%s | %s | accent %u,%u,%u | %s", r.id.c_str(),
                     r.name.c_str(), GetRValue(r.def.accent), GetGValue(r.def.accent),
                     GetBValue(r.def.accent),
                     st.skinId == r.id ? L"使用中" : L"已安装");
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (rows.empty())
        SendMessageW(lb, LB_ADDSTRING, 0,
                     (LPARAM)L"（未安装在线皮肤——下方粘贴 .lxskin 包 URL 导入）");
    if (skipped)
    {
        wchar_t cap[128];
        _snwprintf_s(cap, _TRUNCATE, L"注意：%zu 个 .lxskin 文件不合格已跳过", skipped);
        SetDlgItemTextW(g_hSkinWnd, IDC_SKIN_STATIC, cap);
    }
}

// 选中行 → id（行格式 "id | name | ..."，取首个 " |" 前段；解析不出=空）
std::wstring SelectedSkinId()
{
    HWND lb = GetDlgItem(g_hSkinWnd, IDC_SKIN_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel < 0) return L"";
    wchar_t line[512] = L"";
    SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)line);
    const wchar_t* sep = wcsstr(line, L" | ");
    if (!sep) return L"";
    return std::wstring(line, sep - line);
}

void DoApply(HWND hwnd)
{
    std::wstring id = SelectedSkinId();
    if (id.empty())
    {
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"请先在列表选中一款皮肤");
        return;
    }
    Settings st = LoadSettings();
    st.skinId = id; // skinIndex 不动——它是在线肤被删后的回退档
    SaveSettingsDiff(st);
    LX_LOG(L"ui", L"应用在线皮肤 id=%ls", id.c_str());
    RefreshSkinList();
    SetDlgItemTextW(hwnd, IDC_SKIN_STATIC,
                    L"已应用：下一次候选弹出/面板重绘即换肤（全 UI 即时，无需重启）");
}

void DoUseBuiltin(HWND hwnd)
{
    Settings st = LoadSettings();
    if (st.skinId.empty())
    {
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"当前已是内置皮肤（设置页/候选窗右键可换内置款）");
        return;
    }
    st.skinId.clear();
    SaveSettingsDiff(st);
    LX_LOG(L"ui", L"用回内置皮肤 skin=%u", st.skinIndex);
    RefreshSkinList();
    SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"已用回内置皮肤");
}

void DoDelete(HWND hwnd)
{
    std::wstring id = SelectedSkinId();
    if (id.empty())
    {
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"请先在列表选中要删除的皮肤");
        return;
    }
    if (MessageBoxW(hwnd, (L"删除在线皮肤 " + id + L"？").c_str(), L"确认删除",
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    std::wstring err;
    if (!DeleteSkin(id, &err))
    {
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, (L"删除失败：" + err).c_str());
        return;
    }
    UiReloadSkins();
    Settings st = LoadSettings();
    if (st.skinId == id) // 正引用：显式清（UiCurrentSkin 本也会回退内置，免残留垃圾 id）
    {
        st.skinId.clear();
        SaveSettingsDiff(st);
    }
    LX_LOG(L"ui", L"删除在线皮肤 id=%ls", id.c_str());
    RefreshSkinList();
    SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"已删除（若曾在使用，已自动回退内置皮肤）");
}

struct SkinImportJob { HWND hwnd; std::wstring url; std::wstring sha; };

DWORD WINAPI SkinImportThread(LPVOID p)
{
    SkinImportJob* job = (SkinImportJob*)p;
    SkinImportResult* res = new SkinImportResult(ImportSkinFromUrl(job->url, job->sha));
    if (!IsWindow(job->hwnd))
    {
        delete res; // 窗已灭：直接释放（导入件本身已落盘，下次开窗可见）
        delete job;
        return 0;
    }
    // N9 同型：先登记再投递——窗口在消息处理前销毁时由 WM_DESTROY 领取回收
    if (SkinImportResult* stale = g_skinResSlot.Register(res)) delete stale; // 防御
    if (!PostMessageW(job->hwnd, WM_APP_SKIN_DONE, res->ok ? 1 : 0, (LPARAM)res))
    {
        if (g_skinResSlot.Claim() == res) delete res; // 未被窗口侧领取才回收
    }
    delete job;
    return 0;
}

void DoUrlFetch(HWND hwnd)
{
    if (g_skinImporting) return;
    wchar_t url[2048] = L"", sha[80] = L"";
    GetWindowTextW(g_hSkinUrlEdit, url, 2047);
    GetWindowTextW(g_hSkinShaEdit, sha, 79);
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
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC,
                        L"请先粘贴皮肤包 URL（https，.lxskin 结尾；出网域名白名单为 GitHub）");
        return;
    }
    if (!ValidateSkinUrl(url))
    {
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC,
                        L"URL 不合格：须 https 且以 .lxskin 结尾、不含 @（形闸前置，未出网）");
        return;
    }
    g_skinImporting = true;
    EnableWindow(GetDlgItem(hwnd, IDC_SKIN_URLFETCH), FALSE);
    SetDlgItemTextW(hwnd, IDC_SKIN_STATIC,
                    L"正在后台下载并校验（不阻塞输入与界面）……完成后此处报告结果");
    SkinImportJob* job = new SkinImportJob{ hwnd, url, sha };
    HANDLE h = CreateThread(NULL, 0, SkinImportThread, job, 0, NULL);
    if (h) CloseHandle(h);
    else
    {
        g_skinImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_SKIN_URLFETCH), TRUE);
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, L"无法创建导入线程");
        delete job;
    }
}

LRESULT CALLBACK SkinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SKIN_APPLY: DoApply(hwnd); return 0;
        case IDC_SKIN_USEBUILTIN: DoUseBuiltin(hwnd); return 0;
        case IDC_SKIN_DELETE: DoDelete(hwnd); return 0;
        case IDC_SKIN_URLFETCH: DoUrlFetch(hwnd); return 0;
        case IDC_SKIN_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
        }
        break;
    case WM_APP_SKIN_DONE:
    {
        // N9 同型：只信槽——槽空=所有权已被领取，lParam 不可再用
        SkinImportResult* res = g_skinResSlot.Claim();
        if (!res) return 0;
        g_skinImporting = false;
        EnableWindow(GetDlgItem(hwnd, IDC_SKIN_URLFETCH), TRUE);
        SetDlgItemTextW(hwnd, IDC_SKIN_STATIC, res->message.c_str());
        if (res->ok)
        {
            UiReloadSkins(); // 注册表换入新件（渲染侧读时解析，即时生效）
            LX_LOG(L"ui", L"在线皮肤导入成功 id=%ls", res->id.c_str());
        }
        RefreshSkinList();
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
        if (SkinImportResult* res = g_skinResSlot.Claim()) delete res;
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowSkinWindow(HINSTANCE hInst)
{
    if (g_hSkinWnd)
    {
        ShowWindow(g_hSkinWnd, SW_SHOW);
        SetForegroundWindow(g_hSkinWnd);
        RefreshSkinList();
        return;
    }
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = SkinWndProc;
    wc.hInstance = hInst;
    wc.hIcon = g_hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    if (!g_hBgBrush) g_hBgBrush = CreateSolidBrush(kUiBg);
    wc.hbrBackground = g_hBgBrush;
    wc.lpszClassName = kSkinWndClass;
    RegisterClassExW(&wc);

    UINT dpi = 96;
    if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"))
        dpi = pGetDpi();
    int W = MulDiv(560, dpi, 96), H = MulDiv(400, dpi, 96);
    int btnH = MulDiv(28, dpi, 96);
    int pad = MulDiv(12, dpi, 96);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int winW = W, winH = H;
    {
        RECT rc{ 0, 0, winW, winH };
        if (AdjustWindowRectEx(&rc, style, FALSE, 0))
        { winW = rc.right - rc.left; winH = rc.bottom - rc.top; }
    }
    g_hSkinWnd = CreateWindowExW(0, kSkinWndClass, L"皮肤管理（在线皮肤）", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT, winW, winH, NULL, NULL, hInst, NULL);

    CreateWindowExW(0, L"STATIC",
                    L"在线皮肤随选即用；删除后自动回退内置皮肤。内置 7 款在设置页/候选窗右键切换。",
                    WS_CHILD | WS_VISIBLE,
                    pad, pad, W - pad * 2, btnH, g_hSkinWnd,
                    (HMENU)(INT_PTR)IDC_SKIN_STATIC, hInst, NULL);

    int rowBtnY = H - btnH - pad;
    int listTop = pad + btnH + 6;
    // 在线源行（按钮行上方）：URL + SHA256（可空=侧车）+ 导入按钮
    int urlRowY = rowBtnY - btnH - 6;
    int urlW = (W - pad * 2) * 52 / 100;
    int shaW = (W - pad * 2) * 30 / 100;
    int fetchW = W - pad * 2 - urlW - shaW - 10;
    g_hSkinUrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     pad, urlRowY, urlW, btnH, g_hSkinWnd,
                                     (HMENU)(INT_PTR)IDC_SKIN_URLEDIT, hInst, NULL);
    SendMessageW(g_hSkinUrlEdit, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"在线皮肤包 URL（https://…/*.lxskin，GitHub 白名单）");
    g_hSkinShaEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     pad + urlW + 5, urlRowY, shaW, btnH, g_hSkinWnd,
                                     (HMENU)(INT_PTR)IDC_SKIN_SHAEDIT, hInst, NULL);
    SendMessageW(g_hSkinShaEdit, 0x1501 /*EM_SETCUEBANNERW*/, TRUE,
                 (LPARAM)L"SHA256（可空=自动取侧车 .sha256）");
    CreateWindowExW(0, L"BUTTON", L"URL 导入",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    pad + urlW + shaW + 10, urlRowY, fetchW, btnH, g_hSkinWnd,
                    (HMENU)(INT_PTR)IDC_SKIN_URLFETCH, hInst, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                    pad, listTop, W - pad * 2, urlRowY - listTop - 6, g_hSkinWnd,
                    (HMENU)(INT_PTR)IDC_SKIN_LIST, hInst, NULL);

    struct Btn { const wchar_t* text; int id; };
    Btn btns[] = {
        { L"应用选中", IDC_SKIN_APPLY }, { L"用回内置", IDC_SKIN_USEBUILTIN },
        { L"删除选中", IDC_SKIN_DELETE }, { L"关闭", IDC_SKIN_CLOSE },
    };
    int bw = (W - pad * 2 - 3 * 5) / 4;
    for (int i = 0; i < 4; i++)
        CreateWindowExW(0, L"BUTTON", btns[i].text,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        pad + i * (bw + 5), rowBtnY, bw, btnH, g_hSkinWnd,
                        (HMENU)(INT_PTR)btns[i].id, hInst, NULL);

    if (g_hFont)
    {
        SendMessageW(g_hSkinWnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        for (int id : { IDC_SKIN_STATIC, IDC_SKIN_LIST, IDC_SKIN_URLEDIT, IDC_SKIN_SHAEDIT,
                        IDC_SKIN_URLFETCH, IDC_SKIN_APPLY, IDC_SKIN_USEBUILTIN,
                        IDC_SKIN_DELETE, IDC_SKIN_CLOSE })
            SendMessageW(GetDlgItem(g_hSkinWnd, id), WM_SETFONT, (WPARAM)g_hFont, TRUE);
    }
    RefreshSkinList();
    ShowWindow(g_hSkinWnd, SW_SHOW);
}

} // namespace lingxi
