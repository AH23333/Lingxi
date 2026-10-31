//
// 剪贴板历史实现。结构：
// - 隐藏监听窗（LingxiCbListenWnd，普通隐藏 POPUP——不走 HWND_MESSAGE
//   消息专用窗：WM_CLIPBOARDUPDATE 虽是直投而非广播，消息专用窗场景
//   文档未承诺，保守用普通隐藏窗）承载 WM_CLIPBOARDUPDATE；
// - 历史窗（LingxiCbWnd，blockedui 模板）：列表 + 全文预览 +
//   复制/删除/清空，双击直接复制，Esc 关窗；
// - 存储 clipboard.dat：magic "LXCB" + 版本 + 条数 + 记录
//   [u32 字节长][i64 本地文件时间][UTF-8 内容]，加载全程边界校验，
//   保存走 .tmp+MoveFileExW 原子替换（审计批次 0677401 同模式，
//   进程中途被杀不留半截文件）。
// - 捕获节流：每 5 次落盘一次 + ClipboardFlush（WM_ENDSESSION/
//   WM_DESTROY）补写；删除/清空立即落盘（用户意图不丢）。
//
#include "clipboardui.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

namespace lingxi {

namespace {

constexpr wchar_t kCbWndClass[] = L"LingxiCbWnd";
constexpr wchar_t kCbListenClass[] = L"LingxiCbListenWnd";

constexpr int IDC_CB_LIST = 620;
constexpr int IDC_CB_PREVIEW = 621;
constexpr int IDC_CB_COPY = 622;
constexpr int IDC_CB_DEL = 623;
constexpr int IDC_CB_CLEAR = 624;
constexpr int IDC_CB_CLOSE = 625;
constexpr int IDC_CB_STATUS = 626;

// ---------------- 存储与状态 ----------------

struct CbItem
{
    std::wstring text;
    long long localFt; // 复制时刻（本地时间 FILETIME，仅显示用）
};

std::mutex g_mtx;
std::vector<CbItem> g_items;
bool g_loaded = false;   // 懒加载只做一次
bool g_sandbox = false;
bool g_checked = false;  // 沙箱探测只做一次
uint32_t g_pending = 0;  // 自上次落盘的新捕获数（节流计数）

HWND g_hListen = nullptr;
HWND g_hCbWnd = nullptr;
HWND g_hCbList = nullptr;
WNDPROC g_oldCbListProc = nullptr;
WNDPROC g_oldCbPreviewProc = nullptr;

void RefreshCbList(); // 前向声明（AddCapture 在窗口代码前使用）

void EnsureSandboxChecked()
{
    if (g_checked) return;
    g_checked = true;
    // 读 main.cpp 的快照（而非直读环境变量）：生产宿主启动即清 LINGXI_SETTINGS_NS，
    // e2e 派生的宿主若惰性读环境会误判生产，把剪贴板历史写进 regress 临时 APPDATA
    g_sandbox = WasTestSpawned();
}

std::wstring DataFilePath()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring dir = std::wstring(buf) + L"\\LingxiIme";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\clipboard.dat";
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    return std::wstring(exe) + L"clipboard.dat";
}

std::string WToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, NULL, NULL);
    return s;
}

std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 原子写：.tmp + MoveFileExW（usage.txt/备份同模式）
bool WriteBytesAtomic(const std::wstring& path, const std::string& bytes)
{
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, NULL) &&
              written == bytes.size();
    CloseHandle(h);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    HANDLE h = CreateFileW(DataFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string raw;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n) raw.append(buf, n);
    CloseHandle(h);
    if (raw.size() < 12 || memcmp(raw.data(), "LXCB", 4) != 0) return;
    uint32_t ver = 0, cnt = 0;
    memcpy(&ver, raw.data() + 4, 4);
    memcpy(&cnt, raw.data() + 8, 4);
    if (ver != 1) return;
    size_t pos = 12;
    for (uint32_t i = 0; i < cnt && g_items.size() < kCbMaxEntries; i++)
    {
        if (pos + 12 > raw.size()) break;
        uint32_t len = 0;
        long long ft = 0;
        memcpy(&len, raw.data() + pos, 4);
        memcpy(&ft, raw.data() + pos + 4, 8);
        pos += 12;
        if (len > raw.size() - pos) break; // 截断文件：到此为止，已读部分保留
        CbItem it;
        it.localFt = ft;
        it.text = Utf8ToW(std::string(raw, pos, len));
        pos += len;
        if (!it.text.empty()) g_items.push_back(it);
    }
}

void SaveLocked()
{
    if (!g_loaded) return;
    std::string out("LXCB", 4);
    uint32_t ver = 1;
    uint32_t cnt = (uint32_t)g_items.size();
    out.append((const char*)&ver, 4);
    out.append((const char*)&cnt, 4);
    for (const auto& it : g_items)
    {
        std::string utf = WToUtf8(it.text);
        uint32_t len = (uint32_t)utf.size();
        out.append((const char*)&len, 4);
        out.append((const char*)&it.localFt, 8);
        out.append(utf);
    }
    WriteBytesAtomic(DataFilePath(), out);
    g_pending = 0;
}

// ---------------- 剪贴板读写 ----------------

// 抓取当前剪贴板文本（空=占用重试耗尽/无文本/非文本格式）
std::wstring GrabClipboardText()
{
    for (int i = 0; i < 3; i++)
    {
        if (OpenClipboard(NULL))
        {
            std::wstring text;
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h)
            {
                const wchar_t* p = (const wchar_t*)GlobalLock(h);
                if (p)
                {
                    // GlobalSize 界定可读上限：源若给非终结数据不越界读
                    size_t cap = GlobalSize(h) / sizeof(wchar_t);
                    text.assign(p, wcsnlen(p, cap));
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            return text;
        }
        Sleep(30); // 被占用重试（备份导出 3×30ms 同模式）
    }
    return L"";
}

bool CopyTextToClipboard(HWND hOwner, const std::wstring& text)
{
    for (int i = 0; i < 3; i++)
    {
        if (OpenClipboard(hOwner))
        {
            EmptyClipboard();
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            if (h)
            {
                wchar_t* p = (wchar_t*)GlobalLock(h);
                if (p)
                {
                    wcscpy_s(p, text.size() + 1, text.c_str());
                    GlobalUnlock(h);
                    if (SetClipboardData(CF_UNICODETEXT, h))
                    {
                        CloseClipboard();
                        return true; // 成功后系统接管内存，不可 GlobalFree
                    }
                }
                GlobalFree(h);
            }
            CloseClipboard();
            return false;
        }
        Sleep(30);
    }
    return false;
}

// ---------------- 捕获 ----------------

long long FtNow()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)u.QuadPart;
}

// 新捕获入历史：纯空白跳过、超长截断、与最近一条相同跳过（如自己刚从
// 历史窗复制的回声）、已存在则前移；节流落盘；历史窗开着则刷新列表。
void AddCapture(const std::wstring& text)
{
    if (text.empty()) return;
    bool hasContent = false;
    for (wchar_t ch : text)
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n')
        {
            hasContent = true;
            break;
        }
    if (!hasContent) return; // 纯空白不记
    std::wstring t = text;
    if (t.size() > kCbMaxItemChars) t.resize(kCbMaxItemChars);
    CbItem it;
    it.text = t;
    it.localFt = FtNow();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        LoadLocked();
        if (!g_items.empty() && g_items.front().text == t) return; // 回声去重
        for (size_t i = 0; i < g_items.size(); i++)
            if (g_items[i].text == t)
            {
                g_items.erase(g_items.begin() + (long)i);
                break;
            }
        g_items.insert(g_items.begin(), it);
        if (g_items.size() > kCbMaxEntries) g_items.pop_back();
        if (++g_pending >= 5) SaveLocked();
    }
    if (g_hCbList) RefreshCbList(); // 与历史窗同线程（监听窗在主 UI 线程）
}

// ---------------- 历史窗 ----------------

void FormatCbTime(long long ft, wchar_t* out, size_t cch)
{
    if (ft == 0)
    {
        wcscpy_s(out, cch, L"--:--");
        return;
    }
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)ft;
    FILETIME f;
    f.dwLowDateTime = u.LowPart;
    f.dwHighDateTime = u.HighPart;
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&f, &st))
    {
        wcscpy_s(out, cch, L"--:--");
        return;
    }
    _snwprintf_s(out, cch, _TRUNCATE, L"%02u-%02u %02u:%02u",
                 st.wMonth, st.wDay, st.wHour, st.wMinute);
}

// 列表行预览：换行/制表折叠为空格，超长 64 字符加省略号（全文看预览框）
std::wstring BuildListLine(const CbItem& it)
{
    wchar_t tm[20];
    FormatCbTime(it.localFt, tm, 20);
    std::wstring pv;
    pv.reserve(80);
    for (wchar_t ch : it.text)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        pv += ch;
        if (pv.size() >= 64)
        {
            pv += L"…";
            break;
        }
    }
    return std::wstring(tm) + L"  " + pv;
}

// 预览框显示选中条目全文（未选/空列表给出提示文案）
void UpdateCbPreview(const std::vector<CbItem>& items, int sel)
{
    if (sel < 0 || sel >= (int)items.size())
    {
        SetDlgItemTextW(g_hCbWnd, IDC_CB_PREVIEW,
                        items.empty() ? L"（暂无记录——全局复制的文本会自动出现在这里）"
                                      : L"（在上方列表中选择条目查看全文）");
        return;
    }
    SetDlgItemTextW(g_hCbWnd, IDC_CB_PREVIEW, items[sel].text.c_str());
}

void RefreshCbList()
{
    if (!g_hCbList || !g_hCbWnd) return;
    std::vector<CbItem> items;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        LoadLocked();
        items = g_items;
    }
    int prevSel = (int)SendMessageW(g_hCbList, LB_GETCURSEL, 0, 0);
    SendMessageW(g_hCbList, LB_RESETCONTENT, 0, 0);
    for (const auto& it : items)
        SendMessageW(g_hCbList, LB_ADDSTRING, 0, (LPARAM)BuildListLine(it).c_str());
    int sel = (prevSel >= 0 && prevSel < (int)items.size()) ? prevSel : -1;
    if (sel >= 0) SendMessageW(g_hCbList, LB_SETCURSEL, sel, 0);
    UpdateCbPreview(items, sel);
    wchar_t cap[128] = {};
    _snwprintf_s(cap, _TRUNCATE,
                 L"共 %zu 条（上限 %zu，最近在前；双击条目直接复制回剪贴板）",
                 items.size(), kCbMaxEntries);
    SetDlgItemTextW(g_hCbWnd, IDC_CB_STATUS, cap);
}

void CbCopySelected()
{
    int sel = (int)SendMessageW(g_hCbList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hCbWnd, IDC_CB_STATUS, L"请先在列表中选择要复制的条目");
        return;
    }
    std::wstring text;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (sel < (int)g_items.size()) text = g_items[sel].text;
    }
    if (text.empty() || !CopyTextToClipboard(g_hCbWnd, text))
    {
        MessageBoxW(g_hCbWnd, L"写入剪贴板失败（被其他程序持续占用）。",
                    L"灵犀输入法", MB_OK | MB_ICONWARNING);
        return;
    }
    LX_LOG(L"ui", L"剪贴板历史条目已写回（第 %d 条）", sel);
    SetDlgItemTextW(g_hCbWnd, IDC_CB_STATUS, L"已复制到剪贴板");
}

void CbDeleteSelected()
{
    int sel = (int)SendMessageW(g_hCbList, LB_GETCURSEL, 0, 0);
    if (sel < 0)
    {
        SetDlgItemTextW(g_hCbWnd, IDC_CB_STATUS, L"请先在列表中选择要删除的条目");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (sel < (int)g_items.size())
        {
            g_items.erase(g_items.begin() + (long)sel);
            SaveLocked(); // 删除是用户意图：立即落盘不等节流
        }
    }
    LX_LOG(L"ui", L"剪贴板历史条目删除（第 %d 条）", sel);
    RefreshCbList();
}

void CbClearAll()
{
    if (MessageBoxW(g_hCbWnd, L"将清空全部剪贴板历史（不可恢复），确定？",
                    L"清空剪贴板历史", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_items.clear();
        SaveLocked();
    }
    LX_LOG(L"ui", L"剪贴板历史已清空");
    RefreshCbList();
}

// Esc 关窗（焦点在列表/预览时子类化拦截；与"关闭"同语义=隐藏复用）
LRESULT CALLBACK CbEscListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldCbListProc, h, msg, wp, lp);
}

LRESULT CALLBACK CbEscPreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE)
    {
        ShowWindow(GetParent(h), SW_HIDE);
        return 0;
    }
    return CallWindowProcW(g_oldCbPreviewProc, h, msg, wp, lp);
}

HFONT CbFont()
{
    static HFONT f = nullptr;
    if (!f)
    {
        UINT dpi = 96;
        if (auto pGetDpi = (UINT(WINAPI*)())GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                           "GetDpiForSystem"))
            dpi = pGetDpi();
        f = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }
    return f;
}

HBRUSH CbBgBrush()
{
    static HBRUSH b = CreateSolidBrush(kUiBg);
    return b;
}

LRESULT CALLBACK CbWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED)
            switch (LOWORD(wParam))
            {
            case IDC_CB_COPY: CbCopySelected(); return 0;
            case IDC_CB_DEL: CbDeleteSelected(); return 0;
            case IDC_CB_CLEAR: CbClearAll(); return 0;
            case IDC_CB_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
            }
        else if (HIWORD(wParam) == LBN_SELCHANGE && LOWORD(wParam) == IDC_CB_LIST)
        {
            int sel = (int)SendMessageW(g_hCbList, LB_GETCURSEL, 0, 0);
            std::vector<CbItem> items;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                items = g_items;
            }
            UpdateCbPreview(items, sel);
        }
        else if (HIWORD(wParam) == LBN_DBLCLK && LOWORD(wParam) == IDC_CB_LIST)
        {
            CbCopySelected(); // 双击条目直接复制（历史窗主用法）
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    {
        // 预览框（只读 EDIT 发 CTLCOLORSTATIC）白底；其余标签窗体底
        if (GetWindowLongPtrW((HWND)lParam, GWLP_ID) == IDC_CB_PREVIEW)
        {
            SetBkColor((HDC)wParam, kUiCard);
            SetTextColor((HDC)wParam, kUiText);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        SetBkColor((HDC)wParam, kUiBg);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)CbBgBrush();
    }
    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, kUiCard);
        SetTextColor((HDC)wParam, kUiText);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            ShowWindow(hwnd, SW_HIDE); // 窗体自身持焦点时 Esc 关窗
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 监听窗（隐藏，主 UI 线程）----------------

LRESULT CALLBACK CbListenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CLIPBOARDUPDATE)
    {
        // 无痕模式（批次 G）：不录新历史（既有历史可继续查看/复制/删除）
        if (!IncognitoActive()) AddCapture(GrabClipboardText());
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        RemoveClipboardFormatListener(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool InitClipboardCapture(HINSTANCE hInst)
{
    EnsureSandboxChecked();
    if (g_sandbox) return false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = CbListenProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kCbListenClass;
    RegisterClassW(&wc);
    g_hListen = CreateWindowExW(0, kCbListenClass, L"", WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hListen) return false;
    if (!AddClipboardFormatListener(g_hListen))
    {
        LX_LOG(L"ui", L"剪贴板监听注册失败 err=%u", (unsigned)GetLastError());
        DestroyWindow(g_hListen);
        g_hListen = nullptr;
        return false;
    }
    LX_LOG(L"ui", L"剪贴板历史监听已启动");
    return true;
}

void ShowClipboardWindow(HINSTANCE hInst, HWND hParent)
{
    EnsureSandboxChecked();
    if (g_sandbox) return;
    if (g_hCbWnd)
    {
        ShowWindow(g_hCbWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_hCbWnd);
        RefreshCbList();
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = CbWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CbBgBrush();
    wc.lpszClassName = kCbWndClass;
    RegisterClassW(&wc);

    const int W = 640, H = 540, pad = 14, rowH = 30, btnW = 104;
    g_hCbWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kCbWndClass,
        L"剪贴板历史——全局复制的文本自动记录（Ctrl+Alt+V）",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, W, H, hParent, NULL, hInst, NULL);

    HFONT font = CbFont();
    auto mkLabel = [&](const wchar_t* text, int x, int y, int w) {
        HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, w, 20, g_hCbWnd, NULL, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        return h;
    };

    int y = pad;
    mkLabel(L"历史记录（最近在前；双击条目直接复制回剪贴板）：", pad, y, W - pad * 2);
    y += 24;
    int listH = 230;
    g_hCbList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, y, W - pad * 2 - 12, listH, g_hCbWnd, (HMENU)(INT_PTR)IDC_CB_LIST, hInst, NULL);
    SendMessageW(g_hCbList, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldCbListProc = (WNDPROC)SetWindowLongPtrW(g_hCbList, GWLP_WNDPROC, (LONG_PTR)CbEscListProc);
    y += listH + 12;

    mkLabel(L"选中条目全文：", pad, y, W - pad * 2);
    y += 22;
    int prevH = 130;
    HWND hPrev = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        pad, y, W - pad * 2 - 12, prevH, g_hCbWnd, (HMENU)(INT_PTR)IDC_CB_PREVIEW, hInst, NULL);
    SendMessageW(hPrev, WM_SETFONT, (WPARAM)font, TRUE);
    g_oldCbPreviewProc = (WNDPROC)SetWindowLongPtrW(hPrev, GWLP_WNDPROC, (LONG_PTR)CbEscPreviewProc);
    y += prevH + 10;

    int bx = pad;
    auto mkBtn = [&](const wchar_t* text, int id) {
        HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            bx, y, btnW, rowH, g_hCbWnd, (HMENU)(INT_PTR)id, hInst, NULL);
        SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        bx += btnW + 10;
        return h;
    };
    mkBtn(L"复制选中", IDC_CB_COPY);
    mkBtn(L"删除选中", IDC_CB_DEL);
    mkBtn(L"清空", IDC_CB_CLEAR);
    mkBtn(L"关闭", IDC_CB_CLOSE);
    y += rowH + 10;

    HWND hStat = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, y, W - pad * 2, 22, g_hCbWnd, (HMENU)(INT_PTR)IDC_CB_STATUS, hInst, NULL);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)font, TRUE);

    RefreshCbList();
    ShowWindow(g_hCbWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hCbWnd);
    SetFocus(g_hCbList);
}

void ClipboardFlush()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sandbox || !g_loaded) return;
    if (g_pending > 0) SaveLocked();
}

size_t ClipboardHistoryCount()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_sandbox) return 0;
    LoadLocked();
    return g_items.size();
}

} // namespace lingxi
