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
    void Show(const RECT& caretScreen, const std::vector<std::wstring>& cands,
              bool dark, uint32_t ver);
    void Hide();

    void SetPickHandler(std::function<void(uint32_t ver, uint32_t index)> handler)
    {
        _pick = std::move(handler);
    }

private:
    static LRESULT CALLBACK s_WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void LayoutAndPosition();
    void LayoutMetrics(int* itemH, int* pad);

    HWND _hwnd = nullptr;
    HFONT _font = nullptr;
    bool _dark = true;
    RECT _caret{};          // 屏幕坐标
    uint32_t _ver = 0;
    std::vector<std::wstring> _cands;
    int _width = 0, _height = 0;
    int _lastX = 120, _lastY = 120;
    bool _visible = false;
    CRITICAL_SECTION _cs{};
    bool _csInit = false;
    std::function<void(uint32_t, uint32_t)> _pick;
};

} // namespace lingxi
