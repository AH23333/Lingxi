#pragma once
//
// 编辑会话对象。TSF 要求 RequestEditSession 传入一个 ITfEditSession COM 对象，
// 回调 DoEditSession(TfEditCookie ec) 内执行具体的读/写操作。
// 会话对象自带引用计数，RequestEditSession 返回后由调用方 Release。
//
#include <windows.h>
#include <msctf.h>
#include <string>

#include "service.h"

namespace lingxi {

class CEditSessionBase : public ITfEditSession
{
public:
    CEditSessionBase(CTextService* pSvc, ITfContext* pCtx)
        : _pSvc(pSvc), _pCtx(pCtx), _cRef(1)
    {
        _pSvc->AddRef();
        _pCtx->AddRef();
    }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITfEditSession)))
            *ppv = static_cast<ITfEditSession*>(this);
        if (*ppv) { AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++_cRef; }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG c = --_cRef;
        if (c == 0) delete this;
        return c;
    }

    STDMETHOD(DoEditSession)(TfEditCookie ec) override = 0;

protected:
    virtual ~CEditSessionBase() = default;
    CTextService* _pSvc;
    ITfContext* _pCtx;

private:
    LONG _cRef;
};

// 读取光标前上下文 + 光标屏幕矩形，完成后回调 service->OnContextRead。
class CReadSession : public CEditSessionBase
{
public:
    CReadSession(CTextService* pSvc, ITfContext* pCtx)
        : CEditSessionBase(pSvc, pCtx) {}
    STDMETHOD(DoEditSession)(TfEditCookie ec) override;
};

// 在当前选区插入文本（上屏）。
class CInsertSession : public CEditSessionBase
{
public:
    CInsertSession(CTextService* pSvc, ITfContext* pCtx, const std::wstring& text)
        : CEditSessionBase(pSvc, pCtx), _text(text) {}
    STDMETHOD(DoEditSession)(TfEditCookie ec) override;

private:
    std::wstring _text;
};

// 将光标前 deleteLen 个字符替换为 text（英文前缀补全的上屏语义）。
class CReplaceSession : public CEditSessionBase
{
public:
    CReplaceSession(CTextService* pSvc, ITfContext* pCtx, const std::wstring& text, uint32_t deleteLen)
        : CEditSessionBase(pSvc, pCtx), _text(text), _deleteLen(deleteLen) {}
    STDMETHOD(DoEditSession)(TfEditCookie ec) override;

private:
    std::wstring _text;
    uint32_t _deleteLen;
};

} // namespace lingxi
