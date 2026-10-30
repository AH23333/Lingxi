#include "editsession.h"

#include "../common/log.h"

namespace lingxi {

// CReadSession：读光标前上下文 + 光标矩形。
STDMETHODIMP CReadSession::DoEditSession(TfEditCookie ec)
{
    std::wstring text;
    RECT rc{};
    bool hasRect = false;

    // 说明：密码框等被应用禁用 IME 的上下文，TSF 框架层根本不会把按键/编辑
    // 事件递给本服务（架构性隐私隔离），因此这里无需再检查上下文状态。

    TF_SELECTION sel{};
    ULONG fetched = 0;
    if (FAILED(_pCtx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) ||
        fetched == 0 || !sel.range)
    {
        _pSvc->OnContextRead(text, rc);
        return S_OK;
    }

    ITfRange* pRange = sel.range; // 需要我们 Release
    ITfRange* pBack = nullptr;
    if (SUCCEEDED(pRange->Clone(&pBack)) && pBack)
    {
        // 范围 = [光标-kCtxMaxChars, 光标)
        pBack->Collapse(ec, TF_ANCHOR_END);
        LONG moved = 0;
        pBack->ShiftStart(ec, -kCtxMaxChars, &moved, NULL);

        wchar_t buf[kCtxMaxChars + 1] = L"";
        ULONG cch = 0;
        if (SUCCEEDED(pBack->GetText(ec, 0, buf, kCtxMaxChars, &cch)) && cch > 0)
            text.assign(buf, cch);

        ITfContextView* pView = nullptr;
        if (SUCCEEDED(_pCtx->GetActiveView(&pView)) && pView)
        {
            BOOL clipped = FALSE;
            RECT rcText{};
            if (SUCCEEDED(pView->GetTextExt(ec, pBack, &rcText, &clipped)) &&
                (rcText.right > rcText.left || rcText.bottom > rcText.top))
            {
                rc = rcText;
                hasRect = true;
            }
            pView->Release();
        }
        pBack->Release();
    }
    pRange->Release();

    if (!hasRect)
    {
        // 降级：系统插入符位置（对自绘光标的应用无效，返回 0 由 UI 处理）。
        GUITHREADINFO gti{ sizeof(gti) };
        if (GetGUIThreadInfo(GetCurrentThreadId(), &gti) && gti.hwndCaret)
        {
            POINT pt = { gti.rcCaret.left, gti.rcCaret.bottom };
            ClientToScreen(gti.hwndCaret, &pt);
            rc = { pt.x, pt.y, pt.x, pt.y };
            hasRect = true;
        }
    }

    _pSvc->OnContextRead(text, rc);
    return S_OK;
}

// CInsertSession：在选区插入候选文本。
STDMETHODIMP CInsertSession::DoEditSession(TfEditCookie ec)
{
    ITfInsertAtSelection* pInsert = nullptr;
    if (FAILED(_pCtx->QueryInterface(IID_ITfInsertAtSelection, (void**)&pInsert)))
        return E_FAIL;

    ITfRange* pRange = nullptr;
    HRESULT hr = pInsert->InsertTextAtSelection(ec, 0, _text.c_str(), (LONG)_text.size(), &pRange);
    pInsert->Release();
    if (FAILED(hr) || !pRange)
    {
        if (pRange) pRange->Release();
        if (FAILED(hr))
            LX_LOG(L"ime", L"InsertTextAtSelection 失败 hr=0x%08X", hr);
        return FAILED(hr) ? hr : S_OK;
    }
    // 插入后显式把光标推到文本尾（用户报障根因修复，2026-10-30 晚）：
    // TSF 规范对"插入后的选区/光标"零承诺——MSDN 的 InsertTextAtSelection 只定义
    // ppRange=插入位置，选区是否前进完全取决于宿主 store 的实现。测试宿主
    // （host32/e2e/deep 旧版）全部"好心"自推选区（测试全绿的假象），而 Win11
    // 记事本（WinUI CoreText 适配层）插入后不动选区 → 上屏后光标停在原位，
    // 后续输入全部插到旧光标处（顺序错乱）。显式 SetSelection 收敛宿主差异：
    // 宽松宿主同位幂等无感，严格宿主补上前进。
    TF_SELECTION sel{};
    sel.range = pRange;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    pRange->Collapse(ec, TF_ANCHOR_END);
    HRESULT hs = _pCtx->SetSelection(ec, 1, &sel);
    if (FAILED(hs))
        LX_LOG(L"ime", L"插入后 SetSelection 失败 hr=0x%08X（光标交宿主自理）", hs);
    pRange->Release();
    return hr;
}

// CReplaceSession：替换光标前 deleteLen 个字符。
STDMETHODIMP CReplaceSession::DoEditSession(TfEditCookie ec)
{
    TF_SELECTION sel{};
    ULONG fetched = 0;
    if (FAILED(_pCtx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) ||
        fetched == 0 || !sel.range)
        return E_FAIL;

    ITfRange* pRange = sel.range;
    pRange->Collapse(ec, TF_ANCHOR_END);
    LONG moved = 0;
    pRange->ShiftStart(ec, -(LONG)_deleteLen, &moved, NULL);
    HRESULT hr = pRange->SetText(ec, 0, _text.c_str(), (LONG)_text.size());
    if (SUCCEEDED(hr))
    {
        // 与 CInsertSession 同因：替换后显式把光标推到文本尾——严格宿主
        // （Win11 记事本类）的 SetText 不动选区，不推则光标滞留替换起点。
        pRange->Collapse(ec, TF_ANCHOR_END);
        HRESULT hs = _pCtx->SetSelection(ec, 1, &sel);
        if (FAILED(hs))
            LX_LOG(L"ime", L"替换后 SetSelection 失败 hr=0x%08X", hs);
    }
    pRange->Release();
    if (FAILED(hr))
        LX_LOG(L"ime", L"SetText(替换) 失败 hr=0x%08X", hr);
    return hr;
}

} // namespace lingxi
