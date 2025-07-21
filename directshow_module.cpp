// DirectShow Filter DLL Module Implementation
// COM registration and class factory for Tardsplaya Discontinuity Filter

#include <windows.h>
#include <streams.h>
#include <initguid.h>
#include "directshow_filter.h"
#include "filter_guids.h"

// Module instance handle
HMODULE g_hInst = nullptr;

// Factory template array - required by DirectShow base classes
CFactoryTemplate g_Templates[] = {
    {
        TARDSPLAYA_FILTER_NAME,
        &CLSID_TardsplayaDiscontinuityFilter,
        CTardsplayaDiscontinuityFilter::CreateInstance,
        nullptr,
        nullptr
    }
};

// Template count - required by DirectShow base classes
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// Global COM reference count
LONG g_cServerLocks = 0;

//////////////////////////////////////////////////////////////////////////
// DLL Entry Point
//////////////////////////////////////////////////////////////////////////

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
        DbgInitialise(hInst);
        break;
        
    case DLL_PROCESS_DETACH:
        DbgTerminate();
        break;
    }
    
    return TRUE;
}

//////////////////////////////////////////////////////////////////////////
// COM Export Functions
//////////////////////////////////////////////////////////////////////////

// DllCanUnloadNow - Can the DLL be unloaded?
STDAPI DllCanUnloadNow()
{
    return (g_cServerLocks == 0) ? S_OK : S_FALSE;
}

// DllGetClassObject - Get class factory
STDAPI DllGetClassObject(REFCLSID rClsId, REFIID riid, LPVOID* ppv)
{
    if (!ppv) {
        return E_POINTER;
    }
    
    *ppv = nullptr;
    
    // Check if requesting our filter class
    if (rClsId != CLSID_TardsplayaDiscontinuityFilter) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    
    // Create class factory
    CClassFactory* pClassFactory = new CClassFactory(&g_Templates[0]);
    if (!pClassFactory) {
        return E_OUTOFMEMORY;
    }
    
    // Query for requested interface
    HRESULT hr = pClassFactory->QueryInterface(riid, ppv);
    pClassFactory->Release();
    
    return hr;
}

// DllRegisterServer - Register the filter with Windows
STDAPI DllRegisterServer()
{
    HRESULT hr = S_OK;
    
    // Initialize COM
    hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Register class object
    hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr)) {
        CoUninitialize();
        return hr;
    }
    
    // Register filter with DirectShow
    hr = RegisterFilter();
    
    CoUninitialize();
    return hr;
}

// DllUnregisterServer - Unregister the filter
STDAPI DllUnregisterServer()
{
    HRESULT hr = S_OK;
    
    // Initialize COM
    hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Unregister filter
    hr = UnregisterFilter();
    
    // Unregister class object
    HRESULT hr2 = AMovieDllRegisterServer2(FALSE);
    if (SUCCEEDED(hr)) {
        hr = hr2;
    }
    
    CoUninitialize();
    return hr;
}

//////////////////////////////////////////////////////////////////////////
// Helper Functions
//////////////////////////////////////////////////////////////////////////

// Increment server lock count
void LockServer(BOOL bLock)
{
    if (bLock) {
        InterlockedIncrement(&g_cServerLocks);
    } else {
        InterlockedDecrement(&g_cServerLocks);
    }
}

// Get module handle
HMODULE GetModuleHandle()
{
    return g_hInst;
}

// Check if filter is properly registered
BOOL IsFilterAvailable()
{
    return IsFilterRegistered();
}