#pragma once
//
// 候选窗：置顶、无边框、WS_EX_NOACTIVATE（点击不抢宿主焦点）。
// 数据由管道客户端线程写入（临界区保护），绘制在 UI 主线程。
//
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace lingxi {

class CandWindow
{
public:
    bool Init(HINSTANCE hInst);
    void Shutdown();

    // caretScreen：光标屏幕矩形（全 0 表示未知，沿用上次位置）
    // cands 为全量候选（窗口内部分页，每页 kPageSize 个），ver 变化时回到第一页。
    // composing：当前拼音组合串（B2 回显，纯 UI 内部不走协议）；非空时首行绘制，
    // 候选行整体下移一行，hover/点击行判定同步偏移。空串=无组合（英文补全等）。
    void Show(const RECT& caretScreen, const std::vector<std::wstring>& cands,
              bool dark, uint32_t ver, const std::wstring& composing);
    void Hide();
    // 按设置 candidateOpacity 应用候选窗分层透明度（LWA_ALPHA；100=完全不透明）
    void ApplyOpacity();
    // 翻页：显示全量列表中 pageStart 起始的那一页（ver 不匹配则忽略）。
    void SetPage(uint32_t ver, uint32_t pageStart);
    // 中/英文模式切换的短暂提示：在光标附近显示“中/英”，约 1 秒后自动消失。
    void ShowModeToast(bool english);

    void SetPickHandler(std::function<void(uint32_t ver, uint32_t index)> handler)
    {
        _pick = std::move(handler);
    }

    // 底部工具条按钮（左键行为；固=固定位置、符=符号面板、设=设置，
    // 设右键=循环换肤、固右键=解除固定）。Symbols/Settings 经回调转 main 侧
    // 既有入口（与托盘/热键同路）。
    enum class Btn { None = 0, PrevPage, NextPage, Pin, Symbols, Settings };
    void SetButtonHandler(std::function<void(Btn)> handler)
    {
        _btnCmd = std::move(handler);
    }

    // 托盘切换位置固定：开=固定在当前窗口位置（写注册表），关=恢复跟随光标。
    // 与"固"按钮左键同语义，供托盘菜单勾选项调用。
    void TogglePin();

private:
    static LRESULT CALLBACK s_WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK s_TipWndProc(HWND, UINT, WPARAM, LPARAM); // 工具条按钮 tooltip
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void LayoutAndPosition();
    void LayoutMetrics(int* itemH, int* pad);
    // 按字号（DPI 缩放）重建三支字体；先删旧字体，避免泄漏
    void RebuildFonts(uint32_t fontSize);
    // 应用皮肤（强调色 + 浅色 hover 底色）；越界回退默认翠绿
    void ApplySkin(uint32_t idx);
    // 调用方持锁：返回当前页在全量列表中的起止 [begin,end)
    void CurrentPageRangeLocked(size_t* begin, size_t* end) const;
    // 底部工具条高（DPI 缩放）
    int ToolbarHeight();
    // 客户区坐标 → 工具条按钮（非工具条区域返回 None）
    Btn HitTestButton(int x, int y);
    // 翻页 ±1 页（与滚轮同夹取语义；页内按钮/滚轮共用）
    void DoPageStep(int steps);
    // 工具条按钮 tooltip：懒建常驻、隐藏复用（避免跨线程 DestroyWindow）
    void ShowTip(const wchar_t* text, const POINT& clientPt);
    void HideTip();
    // 手势消息处理（定义在 candwnd_interact.cpp；WndProc 分发壳在 candwnd_core.cpp）
    LRESULT OnMouseMove(HWND hwnd, LPARAM lParam);
    LRESULT OnMouseHover(HWND hwnd, LPARAM lParam);
    LRESULT OnMouseLeave(HWND hwnd);
    LRESULT OnLButtonDown(HWND hwnd, LPARAM lParam);
    BOOL OnSetCursor(HWND hwnd, LPARAM lParam); // TRUE=已处理；FALSE=落 DefWindowProc
    LRESULT OnMouseWheel(WPARAM wParam);
    LRESULT OnLButtonUp(HWND hwnd, LPARAM lParam);
    LRESULT OnRButtonUp(HWND hwnd, LPARAM lParam);
    LRESULT OnXButtonUp(WPARAM wParam);
    LRESULT OnMButtonUp(HWND hwnd, LPARAM lParam);

    HWND _hwnd = nullptr;
    HFONT _font = nullptr;
    HFONT _toastFont = nullptr; // toast 大字（进程内复用）
    HFONT _smallFont = nullptr; // 底部页码/标记小字
    uint32_t _fontSize = 16;    // 当前字号（变化时重建字体）
    uint32_t _fontFace = 0;     // 当前字体族（Settings.uiFont；变化时重建字体）
    uint32_t _skinIndex = 0;    // 当前皮肤（变化时换强调色）
    COLORREF _accent = RGB(16, 124, 16);        // 首行/toast 底色（皮肤）
    COLORREF _hoverLight = RGB(230, 242, 230);  // 浅色 hover 底色（皮肤）
    bool _dark = true;
    RECT _caret{};          // 屏幕坐标
    uint32_t _ver = 0;
    std::vector<std::wstring> _cands; // 全量候选
    std::wstring _composing;          // 当前拼音组合串（B2 首行回显；空=无）
    uint32_t _pageStart = 0;          // 当前页首个候选的全局索引
    int _width = 0, _height = 0;
    int _lastX = 120, _lastY = 120;
    int _opacityApplied = -1; // 上次应用的透明度%，去重 SetLayeredWindowAttributes
    bool _visible = false;
    bool _toastMode = false;   // 正在显示中/英文模式提示
    bool _toastEnglish = false;
    int _hoverRow = -1;        // 鼠标悬停的页内行号（-1 无），用于高亮
    bool _trackingMouse = false;
    int _hoverBtn = 0;         // 悬停中的工具条按钮（Btn 枚举值，0=None）
    bool _dragging = false;    // 拖拽移动窗口中（回显行/工具条空白按住）
    POINT _dragOff{};          // 拖拽：光标→窗口左上角偏移
    bool _dragMoved = false;   // 拖拽发生实际位移（松开时才写入固定位置）
    HWND _ttHwnd = nullptr;    // tooltip 常驻隐藏复用（跨线程只做 ShowWindow）
    wchar_t _ttText[128] = L"";
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    std::function<void(uint32_t, uint32_t)> _pick;
    std::function<void(Btn)> _btnCmd;
};

} // namespace lingxi
