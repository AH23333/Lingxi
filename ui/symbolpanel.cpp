#include "symbolpanel.h"
#include "../common/log.h"
#include "../common/settings.h"
#include "uistyle.h"

#include <windowsx.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>

namespace lingxi {
namespace {

struct Category
{
    const wchar_t* name;          // 标签名（≤3 全角字符宽，kCatTabW 内放得下）
    const wchar_t* const* items;  // 条目：单符号或颜文字串（可含 VS16 变体选择符）
    int count;
    int cellW;                    // 单元格宽（颜文字加宽）
    int cols;                     // 列数
};

// ---------------- 分类内容（2026-10-29 扩充对标 Win+.；10-31 emoji 高频补齐） ----------------

const wchar_t* const kSymCommon[] = {
    L"，", L"。", L"、", L"；", L"：", L"？", L"！", L"…", L"—", L"·", L"〜", L"～",
    L"「", L"」", L"『", L"』", L"《", L"》", L"【", L"】", L"（", L"）", L"〔", L"〕",
    L"‘", L"’", L"“", L"”", L"〖", L"〗",
};

const wchar_t* const kSymMath[] = {
    L"±", L"×", L"÷", L"≠", L"≤", L"≥", L"≈", L"≡", L"≌", L"∞", L"∑", L"∏",
    L"√", L"∂", L"∫", L"∈", L"∉", L"⊆", L"⊇", L"∪", L"∩", L"∧", L"∨", L"⊥",
    L"∥", L"∠", L"∴", L"∵", L"‰", L"°", L"℉", L"℃", L"∆", L"∇",
};

const wchar_t* const kSymCurrency[] = {
    L"￥", L"＄", L"€", L"￠", L"￡", L"¥", L"$", L"¢", L"£", L"₩", L"₽", L"₹",
    L"₫", L"₴", L"₺", L"₦",
};

const wchar_t* const kSymArrow[] = {
    L"←", L"↑", L"→", L"↓", L"↖", L"↗", L"↘", L"↙", L"⇐", L"⇒", L"⇑", L"⇓",
    L"⇔", L"↔", L"↕", L"➜", L"➔", L"►", L"◄", L"▶", L"▼", L"◀", L"▲", L"▽",
    L"◈", L"◇", L"○", L"●", L"◎", L"☆", L"★", L"■", L"□",
};

const wchar_t* const kSymFace[] = {
    L"😀", L"😁", L"😂", L"🤣", L"😊", L"😋", L"😍", L"🥰", L"😘", L"😜", L"🤪", L"🤔",
    L"🤨", L"😐", L"😑", L"😶", L"🙄", L"😏", L"😣", L"😥", L"😭", L"😤", L"😠", L"😡",
    L"🥵", L"🥶", L"😴", L"🤤", L"🥴", L"🤢", L"🤮", L"🤯", L"🥳", L"😎", L"🤓",
    // 10-31 扩充（高频补齐；\u 转义防 VS16 不可见字符在编辑中丢失）
    L"\u1F605", L"\u1F606", L"\u1F917", L"\u1F619", L"\u1F97A", L"\u1F62C", L"\u1F630",
    L"\u1F616", L"\u1F62A", L"\u1F60C", L"\u1F61B", L"\u1F61D", L"\u1F920",
};

const wchar_t* const kSymGesture[] = {
    L"👍", L"👎", L"👌", L"✌️", L"🤞", L"🤟", L"🤘", L"🤙", L"👈", L"👉", L"👆", L"👇",
    L"☝️", L"✋", L"🤚", L"🖐️", L"🖖", L"👋", L"🤝", L"🙏", L"💪", L"🦾", L"✊", L"👏",
    L"🤲",
    // 10-31 扩充（书写/身体语言组；\u 转义防 VS16 丢失）
    L"\u270D\uFE0F", L"\u1F90C", L"\u1F90F", L"\u1F485", L"\u1F646", L"\u1F645",
    L"\u1F647", L"\u1F64B", L"\u1F442", L"\u1F440", L"\u1F463",
};

const wchar_t* const kSymAnimal[] = {
    L"🐱", L"🐶", L"🐭", L"🐹", L"🐰", L"🦊", L"🐻", L"🐼", L"🐨", L"🐯", L"🦁", L"🐮",
    L"🐷", L"🐸", L"🐵", L"🐔", L"🐧", L"🐦", L"🦆", L"🦅", L"🦉", L"🐴", L"🦄", L"🐝",
    L"🦋", L"🐌", L"🐞", L"🐢", L"🐍", L"🐙", L"🦀", L"🐬", L"🐳", L"🐠",
    // 10-31 扩充（走兽/家禽/神话组）
    L"\u1F43A", L"\u1F98C", L"\u1F40E", L"\u1F411", L"\u1F9A2", L"\u1F54A\uFE0F",
    L"\u1F407", L"\u1F408", L"\u1F415", L"\u1F994", L"\u1F43E", L"\u1F409",
    L"\u1F432", L"\u1F99A",
};

const wchar_t* const kSymFood[] = {
    L"🍎", L"🍊", L"🍋", L"🍌", L"🍉", L"🍇", L"🍓", L"🍒", L"🍑", L"🥭", L"🍍", L"🥥",
    L"🥝", L"🍅", L"🥑", L"🍆", L"🥔", L"🥕", L"🌽", L"🌶️", L"🥦", L"🍄", L"🥜", L"🍞",
    L"🥐", L"🧀", L"🍗", L"🍔", L"🍟", L"🍕", L"🌭", L"🥪", L"🍜", L"🍣", L"🍩", L"🍪",
    // 10-31 扩充（主食/甜点/饮品组）
    L"\u1F35A", L"\u1F958", L"\u1F372", L"\u1F95F", L"\u1F364", L"\u1F365",
    L"\u1F960", L"\u1F366", L"\u1F370", L"\u1F382", L"\u1F95B", L"\u2615",
};

const wchar_t* const kSymActivity[] = {
    L"❤️", L"🧡", L"💛", L"💚", L"💙", L"💜", L"🖤", L"🤍", L"💕", L"💖", L"💗", L"💘",
    L"💝", L"🎉", L"🎊", L"🎈", L"🎁", L"🏆", L"⚽", L"🏀", L"🎮", L"🎲", L"🎵", L"🎶",
    L"📞", L"📧", L"💡", L"🔧", L"🔒", L"⏰",
    // 10-31 扩充（出行/地标组）
    L"\u2708\uFE0F", L"\u1F697", L"\u1F68C", L"\u1F6B2", L"\u1F684", L"\u1F6A2",
    L"\u26F5", L"\u1F680", L"\u1F3DD\uFE0F", L"\u1F3D6\uFE0F", L"\u26F0\uFE0F", L"\u1F30B",
};

const wchar_t* const kSymKaomoji[] = {
    L"(^_^)", L"(T_T)", L"(>_<)", L"(^o^)", L"(^3^)", L"(-_-)", L"(._.)", L"(¬_¬)",
    L"(＠_＠)", L"(⊙o⊙)", L"(⊙ω⊙)", L"(Ｔ▽Ｔ)", L"(￣▽￣)", L"(￣ー￣)", L"(ToT)/", L"/(^_^)",
    L"(>3<)", L"(;-;)", L"(X_X)", L"(O_O)", L"(o_o)", L"(Q_Q)", L"(^_~)", L"(^_-)",
};

const wchar_t* const kSymZhuyin[] = { // 拼音声调字母（注音/教学场景）
    L"ā", L"á", L"ǎ", L"à", L"ē", L"é", L"ě", L"è", L"ī", L"í", L"ǐ", L"ì",
    L"ō", L"ó", L"ǒ", L"ò", L"ū", L"ú", L"ǔ", L"ù", L"ǖ", L"ǘ", L"ǚ", L"ǜ",
    L"ü", L"ń", L"ň", L"ê", L"À", L"É", L"Ò",
};

const wchar_t* const kSymGreek[] = { // 希腊字母（数学/物理/公式场景）
    L"α", L"β", L"γ", L"δ", L"ε", L"ζ", L"η", L"θ", L"ι", L"κ", L"λ", L"μ",
    L"ν", L"ξ", L"ο", L"π", L"ρ", L"σ", L"τ", L"υ", L"φ", L"χ", L"ψ", L"ω",
    L"Α", L"Β", L"Γ", L"Δ", L"Ε", L"Ζ", L"Η", L"Θ", L"Ι", L"Κ", L"Λ", L"Μ",
    L"Ν", L"Ξ", L"Ο", L"Π", L"Ρ", L"Σ", L"Τ", L"Υ", L"Φ", L"Χ", L"Ψ", L"Ω",
};

const wchar_t* const kSymBox[] = { // 制表符/方块（文本示意图、进度条场景）
    L"─", L"│", L"┌", L"┐", L"└", L"┘", L"├", L"┤", L"┬", L"┴", L"┼", L"═",
    L"║", L"╔", L"╗", L"╚", L"╝", L"╠", L"╣", L"╦", L"╩", L"╬", L"▁", L"▂",
    L"▃", L"▄", L"▅", L"▆", L"▇", L"█", L"▉", L"▊", L"▌", L"▍", L"▎", L"░",
    L"▒", L"▓",
};

const wchar_t* const kSymIndex[] = { // 序号（列表编号/章节号场景）
    L"①", L"②", L"③", L"④", L"⑤", L"⑥", L"⑦", L"⑧", L"⑨", L"⑩", L"⑪", L"⑫",
    L"Ⅰ", L"Ⅱ", L"Ⅲ", L"Ⅳ", L"Ⅴ", L"Ⅵ", L"Ⅶ", L"Ⅷ", L"Ⅸ", L"Ⅹ", L"⑴", L"⑵",
    L"⑶", L"⑷", L"⑸", L"⑹", L"⑺", L"⑻", L"⑼", L"⑽",
};

const Category kCats[] = {
    { L"常用",   kSymCommon,   (int)(sizeof(kSymCommon) / sizeof(kSymCommon[0])),   38, 12 },
    { L"数学",   kSymMath,     (int)(sizeof(kSymMath) / sizeof(kSymMath[0])),      38, 12 },
    { L"货币",   kSymCurrency, (int)(sizeof(kSymCurrency) / sizeof(kSymCurrency[0])), 38, 12 },
    { L"箭头",   kSymArrow,    (int)(sizeof(kSymArrow) / sizeof(kSymArrow[0])),    38, 12 },
    { L"表情",   kSymFace,     (int)(sizeof(kSymFace) / sizeof(kSymFace[0])),      38, 12 },
    { L"手势",   kSymGesture,  (int)(sizeof(kSymGesture) / sizeof(kSymGesture[0])), 38, 12 },
    { L"动物",   kSymAnimal,   (int)(sizeof(kSymAnimal) / sizeof(kSymAnimal[0])),   38, 12 },
    { L"食物",   kSymFood,     (int)(sizeof(kSymFood) / sizeof(kSymFood[0])),       38, 12 },
    { L"活动",   kSymActivity, (int)(sizeof(kSymActivity) / sizeof(kSymActivity[0])), 38, 12 },
    { L"注音",   kSymZhuyin,   (int)(sizeof(kSymZhuyin) / sizeof(kSymZhuyin[0])),   38, 12 },
    { L"希腊",   kSymGreek,    (int)(sizeof(kSymGreek) / sizeof(kSymGreek[0])),    38, 12 },
    { L"框线",   kSymBox,      (int)(sizeof(kSymBox) / sizeof(kSymBox[0])),         38, 12 },
    { L"序号",   kSymIndex,    (int)(sizeof(kSymIndex) / sizeof(kSymIndex[0])),     38, 12 },
    { L"^_^",   kSymKaomoji,  (int)(sizeof(kSymKaomoji) / sizeof(kSymKaomoji[0])), 104, 4 },
};

constexpr int kCell = 38;      // 单元格高（宽随分类：普通 38 / 颜文字 104）
constexpr int kCatH = 26;
constexpr int kPad = 4;
constexpr int kCatTabW = 48;   // 标签宽（10 分类×48=480，3 全角字标签也放得下）
constexpr int kCloseW = 22;    // 头部右侧 × 关闭按钮宽（搜狗/Win+. 范式：显式出口）
constexpr int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));

// ---------------- "最近"页（动态分类，注册表持久化）----------------
// 首标签 = 最近使用（最新在前，≤12 项）；条目存 g_recentStore，
// 指针数组适配 Category::items 的只读指针约定（store 存活期间指针稳定）。
constexpr size_t kRecentMax = 12;
std::vector<std::wstring> g_recentStore;
std::vector<const wchar_t*> g_recentPtrs;
Category g_recentCat = { L"最近", nullptr, 0, 38, 12 };

void RebuildRecentPtrs()
{
    g_recentPtrs.clear();
    for (const auto& r : g_recentStore) g_recentPtrs.push_back(r.c_str());
}

void LoadRecents()
{
    g_recentStore.clear();
    std::wstring s = LoadSettings().recentSymbols;
    size_t start = 0;
    while (g_recentStore.size() < kRecentMax)
    {
        size_t p = s.find(L'\x1F', start);
        size_t end = (p == std::wstring::npos) ? s.size() : p;
        if (end <= start) break; // 空段/到尾
        g_recentStore.push_back(s.substr(start, end - start));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    RebuildRecentPtrs();
}

void AddRecent(const std::wstring& sym)
{
    if (sym.empty()) return;
    auto it = std::find(g_recentStore.begin(), g_recentStore.end(), sym);
    if (it != g_recentStore.end()) g_recentStore.erase(it);      // 去重前移（最新在前）
    g_recentStore.insert(g_recentStore.begin(), sym);
    if (g_recentStore.size() > kRecentMax) g_recentStore.pop_back();
    RebuildRecentPtrs();
    std::wstring joined;
    for (size_t i = 0; i < g_recentStore.size(); i++)
    {
        if (i) joined += L'\x1F';
        joined += g_recentStore[i];
    }
    Settings st = LoadSettings();
    if (st.recentSymbols != joined)
    {
        st.recentSymbols = joined;
        SaveSettingsDiff(st);
    }
}

// ---------------- "收藏"页（动态分类，注册表持久化）----------------
// 第二标签 = 收藏（右键网格符号 收藏/取消，≤60 项，最新在前）。
// 存储同"最近"页模式：store 保活 + 指针数组适配 Category::items 只读指针约定。
constexpr size_t kFavMax = 60;
std::vector<std::wstring> g_favStore;
std::vector<const wchar_t*> g_favPtrs;
Category g_favCat = { L"收藏", nullptr, 0, 38, 12 };

void RebuildFavPtrs()
{
    g_favPtrs.clear();
    for (const auto& r : g_favStore) g_favPtrs.push_back(r.c_str());
}

void LoadFavs()
{
    g_favStore.clear();
    std::wstring s = LoadSettings().favoriteSymbols;
    size_t start = 0;
    while (g_favStore.size() < kFavMax)
    {
        size_t p = s.find(L'\x1F', start);
        size_t end = (p == std::wstring::npos) ? s.size() : p;
        if (end <= start) break; // 空段/到尾
        g_favStore.push_back(s.substr(start, end - start));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    RebuildFavPtrs();
}

bool IsFavorite(const wchar_t* sym)
{
    for (const auto& f : g_favStore)
        if (f == sym) return true;
    return false;
}

void PersistFavs()
{
    std::wstring joined;
    for (size_t i = 0; i < g_favStore.size(); i++)
    {
        if (i) joined += L'\x1F';
        joined += g_favStore[i];
    }
    Settings st = LoadSettings();
    if (st.favoriteSymbols != joined)
    {
        st.favoriteSymbols = joined;
        SaveSettingsDiff(st);
    }
}

// 右键网格符号的收藏开关：未收藏=置顶加入（超限挤掉最旧），已收藏=移除
void ToggleFavorite(const std::wstring& sym)
{
    if (sym.empty()) return;
    auto it = std::find(g_favStore.begin(), g_favStore.end(), sym);
    bool added = (it == g_favStore.end());
    if (added)
    {
        g_favStore.insert(g_favStore.begin(), sym);
        if (g_favStore.size() > kFavMax) g_favStore.pop_back();
    }
    else
    {
        g_favStore.erase(it);
    }
    RebuildFavPtrs();
    PersistFavs();
    LX_LOG(L"ui", added ? L"符号收藏: %ls" : L"取消符号收藏: %ls", sym.c_str());
}

// ---------------- "日期时间"页（动态分类，打开/切页时现算）----------------
// 日期/时间/星期随开随算（点击上屏当前值）；排在尾标签，不挤占既有序号。
std::vector<std::wstring> g_dtStore;
std::vector<const wchar_t*> g_dtPtrs;
Category g_dtCat = { L"日期", nullptr, 0, 210, 2 }; // "2026年10月31日"≈200px 需宽格

void RebuildDateTimeItems()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const wchar_t* kWeek[] = { L"日", L"一", L"二", L"三", L"四", L"五", L"六" };
    wchar_t buf[64];
    g_dtStore.clear();
    _snwprintf_s(buf, _TRUNCATE, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    g_dtStore.push_back(buf);
    _snwprintf_s(buf, _TRUNCATE, L"%04d年%02d月%02d日", st.wYear, st.wMonth, st.wDay);
    g_dtStore.push_back(buf);
    _snwprintf_s(buf, _TRUNCATE, L"%02d:%02d", st.wHour, st.wMinute);
    g_dtStore.push_back(buf);
    _snwprintf_s(buf, _TRUNCATE, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    g_dtStore.push_back(buf);
    _snwprintf_s(buf, _TRUNCATE, L"星期%ls", kWeek[st.wDayOfWeek]);
    g_dtStore.push_back(buf);
    _snwprintf_s(buf, _TRUNCATE, L"%02d-%02d %02d:%02d",
                 st.wMonth, st.wDay, st.wHour, st.wMinute);
    g_dtStore.push_back(buf);
    g_dtPtrs.clear();
    for (const auto& s : g_dtStore) g_dtPtrs.push_back(s.c_str());
    g_dtCat.items = g_dtPtrs.data();
    g_dtCat.count = (int)g_dtPtrs.size();
}

// 标签序：0="最近"（动态），1="收藏"（动态），2..kCatCount+1=静态分类，
// kCatCount+2="日期时间"（动态）。注册表 lastSymbolCat 仍存静态分类 1 基序
// （kCats 下标+1，与"收藏"页插入前的老版本一致），加载 +1 / 销毁 -1 换算，免迁移。
constexpr int kTabCount = kCatCount + 3;      // 含"最近""收藏""日期时间"三动态页
constexpr int kFavTabIndex = 1;               // "收藏"页标签序号（紧跟"最近"）
constexpr int kDtTabIndex = kCatCount + 2;    // "日期时间"页标签序号

const Category& CatAt(int i)
{
    if (i <= 0)
    {
        g_recentCat.items = g_recentPtrs.empty() ? nullptr : g_recentPtrs.data();
        g_recentCat.count = (int)g_recentPtrs.size();
        return g_recentCat;
    }
    if (i == kFavTabIndex)
    {
        g_favCat.items = g_favPtrs.empty() ? nullptr : g_favPtrs.data();
        g_favCat.count = (int)g_favPtrs.size();
        return g_favCat;
    }
    if (i == kDtTabIndex) return g_dtCat;
    return kCats[i - 2];
}

// × 关闭按钮命中区（头部最右）
RECT CloseBtnRect()
{
    return { kPad + kCatTabW * kTabCount + 2, 2,
             kPad + kCatTabW * kTabCount + kCloseW - 2, kCatH - 1 };
}

HWND g_hPanel = nullptr;
int g_cat = 0;
int g_hoverIdx = -1; // 网格悬停高亮（-1=无；hover 高亮 + 手型光标）
bool g_mouseTracked = false;
HFONT g_font = nullptr;
HFONT g_fontSmall = nullptr; // 收藏★角标用小号字（12px，与 20px 主字体并存）
std::function<void(const std::wstring&)> g_onInsert;

const Category& CurCat() { return CatAt(g_cat); }

int PanelHeight()
{
    const Category& c = CurCat();
    int rows = (c.count + c.cols - 1) / c.cols;
    if (rows < 1) rows = 1; // "最近"空态也保一行高（画提示文字）
    return kCatH + kPad + rows * kCell + kPad;
}

int PanelWidth() { return kPad + kCatTabW * kTabCount + kCloseW + kPad; }

// 网格水平居中于面板（不同分类网格宽不同）
int GridX() { return kPad + (PanelWidth() - 2 * kPad - CurCat().cellW * CurCat().cols) / 2; }

// 网格命中：返回分类内条目序号（-1=头部/格间/页外）；供点击/hover/光标三路复用
int HitItem(int x, int y)
{
    if (y < kCatH) return -1;
    const Category& cat = CurCat();
    int gx = GridX();
    int col = (x - gx) / cat.cellW;
    int row = (y - kCatH - kPad) / kCell;
    int idx = row * cat.cols + col;
    if (col < 0 || col >= cat.cols || x < gx || idx < 0 || idx >= cat.count) return -1;
    return idx;
}

void ClosePanel()
{
    if (g_hPanel) DestroyWindow(g_hPanel);
    g_hPanel = nullptr;
}

LRESULT CALLBACK PanelWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (y < kCatH)
        {
            // × 关闭按钮（头部最右）：显式出口（NOACTIVATE 收不到 Esc，
            // 右键关闭又无提示——用户报"面板关不掉"的两个入口缺口）
            RECT cb = CloseBtnRect();
            if (x >= cb.left && x <= cb.right && y >= cb.top && y <= cb.bottom)
            {
                ClosePanel();
                return 0;
            }
            int c = (x - kPad) / kCatTabW;
            if (c >= 0 && c < kTabCount && c != g_cat)
            {
                g_cat = c;
                if (c == kDtTabIndex) RebuildDateTimeItems(); // 时间页切到时现算保鲜
                g_hoverIdx = -1; // 换页旧 hover 高亮作废
                SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                             SWP_NOMOVE | SWP_NOACTIVATE);
                UiApplyRoundCorners(h, PanelWidth(), PanelHeight()); // RGN 路径随尺寸重设
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        const Category& cat = CurCat();
        int gx = GridX();
        int col = (x - gx) / cat.cellW;
        int row = (y - kCatH - kPad) / kCell;
        int idx = row * cat.cols + col;
        if (col >= 0 && col < cat.cols && x >= gx && idx >= 0 && idx < cat.count)
        {
            std::wstring item(cat.items[idx]);
            if (g_onInsert) g_onInsert(item);
            // "最近"页记录（去重前移+落盘）；"日期时间"页不进最近（随开随算，长串也挤坏 38px 格）
            if (g_cat != kDtTabIndex) AddRecent(item);
            if (g_cat == 0) InvalidateRect(h, NULL, FALSE); // 正看"最近"页则即时刷新
            // 插入后保留面板（便于连续输入）；点击符号不抢焦点
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < kCatH)
        {
            // 右键"最近"标签 = 清空最近记录
            if (x >= kPad && x < kPad + kCatTabW)
            {
                g_recentStore.clear();
                g_recentPtrs.clear();
                Settings st = LoadSettings();
                if (!st.recentSymbols.empty())
                {
                    st.recentSymbols.clear();
                    SaveSettingsDiff(st);
                }
                InvalidateRect(h, NULL, TRUE); // 清空后空态提示立现
                LX_LOG(L"ui", L"右键\"最近\"标签清空记录");
                return 0;
            }
            // 右键"收藏"标签 = 确认后清空收藏（收藏是用户主动沉淀，比"最近"贵重，须确认）
            if (x >= kPad + kFavTabIndex * kCatTabW && x < kPad + (kFavTabIndex + 1) * kCatTabW)
            {
                if (!g_favStore.empty() &&
                    MessageBoxW(h, L"清空全部收藏的符号？", L"灵犀输入法",
                                MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) == IDOK)
                {
                    g_favStore.clear();
                    g_favPtrs.clear();
                    PersistFavs();
                    SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                                 SWP_NOMOVE | SWP_NOACTIVATE);
                    UiApplyRoundCorners(h, PanelWidth(), PanelHeight());
                    InvalidateRect(h, NULL, TRUE); // 空态提示立现 + 行数收缩随动
                    LX_LOG(L"ui", L"右键\"收藏\"标签清空收藏");
                }
                return 0;
            }
        }
        else
        {
            // 右键网格条目 = 收藏/取消收藏（"日期"页条目随开随算，不收藏）
            int idx = HitItem(x, y);
            if (idx >= 0 && g_cat != kDtTabIndex)
            {
                ToggleFavorite(std::wstring(CurCat().items[idx]));
                if (g_cat == kFavTabIndex)
                {
                    // 正看"收藏"页：增删条目行数变，面板高度/圆角随动
                    SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                                 SWP_NOMOVE | SWP_NOACTIVATE);
                    UiApplyRoundCorners(h, PanelWidth(), PanelHeight());
                    InvalidateRect(h, NULL, TRUE);
                }
                else
                {
                    InvalidateRect(h, NULL, FALSE); // 其他页仅★角标即时刷新
                }
                return 0;
            }
        }
        ClosePanel();
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        // 网格悬停高亮（Win 标准点击反馈；无高亮时"能不能点"不可感知）
        int idx = HitItem(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx != g_hoverIdx)
        {
            g_hoverIdx = idx;
            InvalidateRect(h, NULL, FALSE);
        }
        if (!g_mouseTracked)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            if (TrackMouseEvent(&tme)) g_mouseTracked = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouseTracked = false;
        if (g_hoverIdx != -1)
        {
            g_hoverIdx = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        // 网格条目上用手型（可点暗示），其余区域走系统默认
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (HitItem(pt.x, pt.y) >= 0)
            {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEWHEEL:
    {
        // 滚轮换分类标签（12 页横向不便点击时的快捷手势；上滚=左一页）
        int d = (short)GET_WHEEL_DELTA_WPARAM(wp);
        if (d == 0) return 0;
        int nc = (g_cat + (d > 0 ? -1 : 1) + kTabCount) % kTabCount;
        if (nc != g_cat)
        {
            g_cat = nc;
            if (nc == kDtTabIndex) RebuildDateTimeItems(); // 时间页切到时现算保鲜
            g_hoverIdx = -1; // 换页旧 hover 高亮作废
            SetWindowPos(h, HWND_TOPMOST, 0, 0, PanelWidth(), PanelHeight(),
                         SWP_NOMOVE | SWP_NOACTIVATE);
            UiApplyRoundCorners(h, PanelWidth(), PanelHeight());
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        const SkinDef& skin = UiSkin(LoadSettings().skinIndex);

        // WinUI 浮层：白底 + 8px 圆角浅描边（RGN/DWM 负责裁圆角）
        HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
        FillRect(hdc, &rc, white);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, kUiFlyoutBorder);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 16, 16);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, g_font);

        // 分类按钮：选中=皮肤浅底药丸 + 强调色字；未选中=次级灰字
        for (int i = 0; i < kTabCount; i++)
        {
            RECT cr{ kPad + i * kCatTabW, 2, kPad + (i + 1) * kCatTabW, kCatH - 1 };
            if (i == g_cat)
            {
                RECT pr = cr;
                InflateRect(&pr, -2, -1);
                HBRUSH hl = CreateSolidBrush(skin.hoverLight);
                HGDIOBJ oldBrush = SelectObject(hdc, hl);
                HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
                int pill = (pr.bottom - pr.top) * 2; // 全高药丸圆角
                RoundRect(hdc, pr.left, pr.top, pr.right + 1, pr.bottom + 1, pill, pill);
                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(hl);
                SetTextColor(hdc, skin.accent);
            }
            else
            {
                SetTextColor(hdc, kUiText2);
            }
            DrawTextW(hdc, CatAt(i).name, -1, &cr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // × 关闭按钮：次级灰，hover 语义省略（静态可见即可）
        RECT cb = CloseBtnRect();
        SetTextColor(hdc, kUiText2);
        DrawTextW(hdc, L"\u00D7", -1, &cb,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 网格符号（字符串条目：单符号/颜文字/含 VS16 变体选择符的 emoji）
        const Category& cat = CurCat();
        int gx = GridX();
        for (int i = 0; i < cat.count; i++)
        {
            int c = i % cat.cols;
            int r = i / cat.cols;
            RECT cell{ gx + c * cat.cellW, kCatH + kPad + r * kCell,
                       gx + (c + 1) * cat.cellW, kCatH + kPad + (r + 1) * kCell };
            if (i == g_hoverIdx) // 悬停高亮：皮肤浅底 + 强调色字（与选中标签同语言）
            {
                RECT hl = cell;
                InflateRect(&hl, -1, -1);
                HBRUSH hb = CreateSolidBrush(skin.hoverLight);
                FillRect(hdc, &hl, hb);
                DeleteObject(hb);
                SetTextColor(hdc, skin.accent);
            }
            else
            {
                SetTextColor(hdc, kUiText);
            }
            DrawTextW(hdc, cat.items[i], -1, &cell,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            // ★角标：已收藏符号在格右上角标星（"收藏"页本身全是收藏，不重复标）
            if (g_cat != kFavTabIndex && IsFavorite(cat.items[i]))
            {
                RECT star{ cell.right - 14, cell.top + 1, cell.right - 2, cell.top + 13 };
                HGDIOBJ oldSmall = SelectObject(hdc, g_fontSmall);
                SetTextColor(hdc, skin.accent);
                DrawTextW(hdc, L"★", -1, &star, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, oldSmall);
            }
        }

        // "最近"页空态提示（首行网格区居中）
        if (g_cat == 0 && g_recentPtrs.empty())
        {
            RECT hint{ kPad, kCatH + kPad, rc.right - kPad, kCatH + kPad + kCell };
            SetTextColor(hdc, kUiText2);
            DrawTextW(hdc, L"暂无最近使用（点击任意符号自动记录，最多 12 个）",
                      -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // "收藏"页空态提示（首行网格区居中）
        if (g_cat == kFavTabIndex && g_favPtrs.empty())
        {
            RECT hint{ kPad, kCatH + kPad, rc.right - kPad, kCatH + kPad + kCell };
            SetTextColor(hdc, kUiText2);
            DrawTextW(hdc, L"暂无收藏（右键任意符号收藏，最多 60 个）",
                      -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, oldFont);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        // 记忆上次查看的静态分类标签（动态页"最近/收藏/日期"不记忆——各自有更优默认）。
        // 注册表仍存静态分类 1 基序（kCats 下标+1）；静态页现居标签序 2..kCatCount+1，
        // 存取两侧各 ±1 换算，老版本写入的值无须迁移。
        if (g_cat >= 2 && g_cat <= kCatCount + 1)
        {
            uint32_t catIdx = (uint32_t)(g_cat - 1);
            Settings st = LoadSettings();
            if (st.lastSymbolCat != catIdx)
            {
                st.lastSymbolCat = catIdx;
                SaveSettingsDiff(st);
            }
        }
        g_hPanel = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void ShowSymbolPanel(HINSTANCE hInst, std::function<void(const std::wstring&)> onInsert)
{
    // 热键/托盘重按 = 切替关闭（搜狗范式）：旧实现已开只置顶，用户重按热键
    // 面板纹丝不动，叠加"仅右键可关且无提示"——体感"面板关不掉"。
    if (g_hPanel)
    {
        ClosePanel();
        return;
    }
    g_onInsert = std::move(onInsert);

    // "最近"/"收藏"页载入 + 上次静态分类记忆：动态页保持智能默认（有记录开
    // "最近"，空则开"常用"），静态分类（数学/表情/…）则恢复上次查看页。
    LoadRecents();
    LoadFavs();
    RebuildDateTimeItems(); // "日期时间"页现算（面板开着时间会走，切页时再刷新）
    g_hoverIdx = -1;
    g_mouseTracked = false;
    uint32_t lastCat = LoadSettings().lastSymbolCat;
    if (lastCat >= 1 && lastCat <= (uint32_t)kCatCount)
        g_cat = (int)lastCat + 1; // 静态页现居标签序 2..kCatCount+1（"收藏"页插序 1）
    else
        g_cat = g_recentPtrs.empty() ? 2 : 0; // 默认"常用"（现序 2）/ 有记录开"最近"

    WNDCLASSW wc{};
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.style = CS_DROPSHADOW; // WinUI 浮层级浅投影
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"LingxiSymbolPanel";
    RegisterClassW(&wc);

    // 字体进程内复用：仅首次创建，避免每次开面板泄漏一个 HFONT。
    if (!g_font)
        g_font = CreateFontW(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    if (!g_fontSmall)
        g_fontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    // 按鼠标所在显示器（含副屏负坐标）的工作区钳位
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT w = mi.rcWork;
    int x = pt.x;
    int y = pt.y;
    if (x + PanelWidth() > w.right) x = w.right - PanelWidth();
    if (x < w.left) x = w.left;
    if (y + PanelHeight() > w.bottom) y = w.bottom - PanelHeight();
    if (y < w.top) y = w.top;

    g_hPanel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"LingxiSymbolPanel", L"符号面板",
        WS_POPUP, x, y, PanelWidth(), PanelHeight(),
        0, 0, hInst, 0);
    if (g_hPanel)
    {
        UiApplyRoundCorners(g_hPanel, PanelWidth(), PanelHeight()); // 圆角：描边自绘
        ShowWindow(g_hPanel, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hPanel);
    }
}

} // namespace lingxi
