//
// ImeCore.dll 入口与注册。
// 注册三步（参照微软官方 textservice 样本 / text-service-registration 文档）：
//   1. COM CLSID -> InprocServer32（HKLM，无管理员权限时回退 HKCU per-user）
//   2. ITfInputProcessorProfiles::Register + AddLanguageProfile（en-US / zh-CN）
//   3. ITfCategoryMgr::RegisterCategory(GUID_TFCAT_TIP_KEYBOARD)
//
#include <windows.h>
#include <objbase.h>
#include <olectl.h>
#include <msctf.h>

#include "service.h"
#include "../common/guids.h"
#include "../common/log.h"

HINSTANCE g_hInst = nullptr;
LONG g_cComponents = 0;
static LONG g_cLock = 0;

using lingxi::CTextService;

// ==================== 类厂 ====================

class CClassFactory : public IClassFactory
{
public:
    CClassFactory() : _cRef(1) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
            *ppv = (IClassFactory*)this;
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
    STDMETHOD(CreateInstance)(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CTextService* pSvc = new CTextService();
        if (!pSvc) return E_OUTOFMEMORY;
        HRESULT hr = pSvc->QueryInterface(riid, ppv);
        pSvc->Release();
        return hr;
    }
    STDMETHOD(LockServer)(BOOL fLock) override
    {
        if (fLock) InterlockedIncrement(&g_cLock);
        else InterlockedDecrement(&g_cLock);
        return S_OK;
    }

private:
    LONG _cRef;
};

// ==================== COM 导出 ====================

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_LingxiTextService))
        return CLASS_E_CLASSNOTAVAILABLE;
    CClassFactory* pFactory = new CClassFactory();
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cComponents == 0 && g_cLock == 0) ? S_OK : S_FALSE;
}

// ==================== 注册 ====================

static const wchar_t kServiceDesc[] = L"灵犀 AI 输入法";
static const wchar_t kThreadingModel[] = L"Apartment";

static void GuidToString(REFGUID guid, wchar_t* out)
{
    // 输出 "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}"（38 字符 + 结尾 0）
    static const wchar_t hex[] = L"0123456789ABCDEF";
    wcscpy_s(out, 39, L"{");
    wchar_t* p = out + 1;
    const unsigned char* bytes = (const unsigned char*)&guid;
    static const int map[] = {3,2,1,0, -1, 5,4, -1, 7,6, -1, 8,9, -1, 10,11,12,13,14,15};
    for (int i = 0; i < 20; i++)
    {
        if (map[i] < 0) *p++ = L'-';
        else
        {
            *p++ = hex[(bytes[map[i]] >> 4) & 0xF];
            *p++ = hex[bytes[map[i]] & 0xF];
        }
    }
    *p++ = L'}';
    *p = L'\0';
}

// 步骤 1：COM CLSID 注册。优先 HKLM；无管理员权限则回退 HKCU（per-user）。
static BOOL RegisterServer()
{
    wchar_t clsidStr[64];
    GuidToString(CLSID_LingxiTextService, clsidStr);

    wchar_t dllPath[MAX_PATH] = L"";
    GetModuleFileNameW(g_hInst, dllPath, MAX_PATH);

    const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int i = 0; i < 2; i++)
    {
        wchar_t sub[96];
        _snwprintf_s(sub, _TRUNCATE, L"Software\\Classes\\CLSID\\%s", clsidStr);
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(roots[i], sub, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
            continue;

        RegSetValueExW(hKey, NULL, 0, REG_SZ,
                       (const BYTE*)kServiceDesc, (DWORD)((wcslen(kServiceDesc) + 1) * sizeof(wchar_t)));

        HKEY hSub = nullptr;
        BOOL ok = FALSE;
        if (RegCreateKeyExW(hKey, L"InprocServer32", 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE, NULL, &hSub, NULL) == ERROR_SUCCESS)
        {
            // 路径含空格（如本机 "D:\Visual Studio Code\..."）必须加引号，
            // 否则 COM/CTF 解析 InprocServer32 时会被截断。
            wchar_t quoted[MAX_PATH + 2];
            _snwprintf_s(quoted, _TRUNCATE, L"\"%s\"", dllPath);
            RegSetValueExW(hSub, NULL, 0, REG_SZ,
                           (const BYTE*)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hSub, L"ThreadingModel", 0, REG_SZ,
                           (const BYTE*)kThreadingModel, (DWORD)((wcslen(kThreadingModel) + 1) * sizeof(wchar_t)));
            RegCloseKey(hSub);
            ok = TRUE;
        }
        RegCloseKey(hKey);
        if (ok)
        {
            LX_LOG(L"reg", L"CLSID 注册于 %s", i == 0 ? L"HKLM" : L"HKCU(per-user)");
            return TRUE;
        }
    }
    return FALSE;
}

// 步骤 2：TSF profile 注册（en-US 0x0409 主配置 + zh-CN 0x0804 便于中文测试）。
static BOOL RegisterProfiles()
{
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    ITfInputProcessorProfiles* pProfiles = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                  IID_ITfInputProcessorProfiles, (void**)&pProfiles);
    if (FAILED(hr))
    {
        LX_LOG(L"reg", L"CLSID_TF_InputProcessorProfiles 创建失败 0x%08X", hr);
        if (SUCCEEDED(hrInit)) CoUninitialize();
        return FALSE;
    }

    hr = pProfiles->Register(CLSID_LingxiTextService);
    LX_LOG(L"reg", L"InputProcessorProfiles::Register: 0x%08X", hr);
    if (SUCCEEDED(hr))
    {
        wchar_t dllPath[MAX_PATH] = L"";
        GetModuleFileNameW(g_hInst, dllPath, MAX_PATH);

        HRESULT hrEn = pProfiles->AddLanguageProfile(CLSID_LingxiTextService, 0x0409,
                                                     GUID_LingxiProfile_enUS,
                                                     kServiceDesc, (ULONG)wcslen(kServiceDesc),
                                                     dllPath, (ULONG)wcslen(dllPath), 0);
        HRESULT hrZh = pProfiles->AddLanguageProfile(CLSID_LingxiTextService, 0x0804,
                                                     GUID_LingxiProfile_zhCN,
                                                     kServiceDesc, (ULONG)wcslen(kServiceDesc),
                                                     dllPath, (ULONG)wcslen(dllPath), 0);
        LX_LOG(L"reg", L"AddLanguageProfile en-US=0x%08X zh-CN=0x%08X", hrEn, hrZh);
        if (FAILED(hrEn) && FAILED(hrZh)) hr = hrEn;
    }

    pProfiles->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return SUCCEEDED(hr);
}

// 步骤 3：键盘 TIP 类别注册。
static BOOL RegisterCategories()
{
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    ITfCategoryMgr* pCat = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
                                  IID_ITfCategoryMgr, (void**)&pCat);
    if (SUCCEEDED(hr))
    {
        hr = pCat->RegisterCategory(CLSID_LingxiTextService,
                                    GUID_TFCAT_TIP_KEYBOARD,
                                    CLSID_LingxiTextService);
        LX_LOG(L"reg", L"RegisterCategory(TIP_KEYBOARD): 0x%08X", hr);
        pCat->Release();
    }

    if (SUCCEEDED(hrInit)) CoUninitialize();
    return SUCCEEDED(hr);
}

// 返回 S_OK 或失败 HRESULT：CTF 的 Unregister 只写 HKLM，无管理员必失败
// （PITFALLS #4）——失败必须向上传播，否则 regsvr32 /u 报"Done"造成假成功，
// 残留的 TIP/Category 键配合已删的 CLSID 会形成"幽灵输入法"。
static HRESULT UnregisterProfiles()
{
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HRESULT hr = S_OK;
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, NULL, CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfiles, (void**)&pProfiles)))
    {
        hr = pProfiles->Unregister(CLSID_LingxiTextService);
        if (FAILED(hr))
            LX_LOG(L"reg", L"InputProcessorProfiles::Unregister: 0x%08X（无管理员时预期失败）", hr);
        pProfiles->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return hr;
}

static HRESULT UnregisterCategories()
{
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HRESULT hr = S_OK;
    ITfCategoryMgr* pCat = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, NULL, CLSCTX_INPROC_SERVER,
                                   IID_ITfCategoryMgr, (void**)&pCat)))
    {
        hr = pCat->UnregisterCategory(CLSID_LingxiTextService,
                                      GUID_TFCAT_TIP_KEYBOARD,
                                      CLSID_LingxiTextService);
        if (FAILED(hr))
            LX_LOG(L"reg", L"UnregisterCategory: 0x%08X（无管理员时预期失败）", hr);
        pCat->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return hr;
}

// 删除 COM CLSID 键。返回 ERROR_SUCCESS 表示至少一侧删除成功。
static LSTATUS UnregisterServer()
{
    wchar_t clsidStr[64];
    GuidToString(CLSID_LingxiTextService, clsidStr);
    wchar_t sub[96];
    _snwprintf_s(sub, _TRUNCATE, L"Software\\Classes\\CLSID\\%s", clsidStr);
    LSTATUS rc1 = RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub);
    LSTATUS rc2 = RegDeleteTreeW(HKEY_CURRENT_USER, sub);
    if (rc1 == ERROR_SUCCESS || rc2 == ERROR_SUCCESS) return ERROR_SUCCESS;
    // 两侧键都不存在 = 本就未注册，幂等视为成功
    if (rc1 == ERROR_FILE_NOT_FOUND && rc2 == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    return rc1 != ERROR_FILE_NOT_FOUND ? rc1 : rc2;
}

STDAPI DllRegisterServer()
{
    BOOL fServer = RegisterServer();
    BOOL fProfiles = RegisterProfiles();
    BOOL fCategories = RegisterCategories();
    LX_LOG(L"reg", L"DllRegisterServer: server=%d profiles=%d categories=%d",
           fServer, fProfiles, fCategories);

    if (fServer && fProfiles && fCategories)
        return S_OK;

    DllUnregisterServer(); // 清理半注册状态
    return SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer()
{
    // 任一 CTF 注销失败 → SELFREG_E_CLASS：让 regsvr32 /u 显式报错，
    // 提醒用户以管理员重跑，而不是假 "Done" 留下幽灵 TIP 键。
    HRESULT hrCat = UnregisterCategories();
    HRESULT hrProf = UnregisterProfiles();
    LSTATUS rcSrv = UnregisterServer();
    LX_LOG(L"reg", L"DllUnregisterServer: cat=0x%08X profiles=0x%08X server=%ld",
           (uint32_t)hrCat, (uint32_t)hrProf, (long)rcSrv);
    if (SUCCEEDED(hrCat) && SUCCEEDED(hrProf) && rcSrv == ERROR_SUCCESS)
        return S_OK;
    return SELFREG_E_CLASS;
}

// ==================== DllMain ====================

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hInst = hInstance;
        DisableThreadLibraryCalls(hInstance);
        lingxi::InitLogging(); // 按 LINGXI_LOG 环境变量开启热路径诊断日志
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
