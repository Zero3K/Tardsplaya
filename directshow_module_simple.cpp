// Simplified DirectShow Filter DLL Module for MinGW-w64

#include <windows.h>
#include <objbase.h>
#include "directshow_filter_simple.h"
#include "filter_guids.h"

// Global variables
HMODULE g_hInst = nullptr;
LONG g_cServerLocks = 0;

// DLL Entry Point
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
        break;
        
    case DLL_PROCESS_DETACH:
        break;
    }
    
    return TRUE;
}

// COM Export Functions
extern "C" {

// DllCanUnloadNow - Can the DLL be unloaded?
__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow()
{
    return (g_cServerLocks == 0) ? S_OK : S_FALSE;
}

// DllGetClassObject - Get class factory
__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID rClsId, REFIID riid, LPVOID* ppv)
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
    CClassFactory* pClassFactory = new CClassFactory();
    if (!pClassFactory) {
        return E_OUTOFMEMORY;
    }
    
    // Query for requested interface
    HRESULT hr = pClassFactory->QueryInterface(riid, ppv);
    pClassFactory->Release();
    
    return hr;
}

// DllRegisterServer - Register the filter with Windows
__declspec(dllexport) HRESULT WINAPI DllRegisterServer()
{
    HRESULT hr = S_OK;
    
    // Initialize COM
    hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Register COM class in registry
    HKEY hkey = nullptr;
    
    // Register CLSID
    WCHAR clsidStr[64];
    StringFromGUID2(CLSID_TardsplayaDiscontinuityFilter, clsidStr, 64);
    
    WCHAR keyPath[256];
    swprintf_s(keyPath, L"CLSID\\%s", clsidStr);
    
    LONG result = RegCreateKeyEx(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 
                               REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr);
    
    if (result == ERROR_SUCCESS) {
        RegSetValueEx(hkey, nullptr, 0, REG_SZ, 
                     (BYTE*)TARDSPLAYA_FILTER_NAME, 
                     (wcslen(TARDSPLAYA_FILTER_NAME) + 1) * sizeof(WCHAR));
        RegCloseKey(hkey);
        
        // Register InprocServer32
        WCHAR dllPath[MAX_PATH];
        GetModuleFileName(g_hInst, dllPath, MAX_PATH);
        
        swprintf_s(keyPath, L"CLSID\\%s\\InprocServer32", clsidStr);
        result = RegCreateKeyEx(HKEY_CLASSES_ROOT, keyPath, 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr);
        
        if (result == ERROR_SUCCESS) {
            RegSetValueEx(hkey, nullptr, 0, REG_SZ, 
                         (BYTE*)dllPath, (wcslen(dllPath) + 1) * sizeof(WCHAR));
            
            const WCHAR* threadingModel = L"Both";
            RegSetValueEx(hkey, L"ThreadingModel", 0, REG_SZ,
                         (BYTE*)threadingModel, (wcslen(threadingModel) + 1) * sizeof(WCHAR));
            
            RegCloseKey(hkey);
        }
    }
    
    // Register filter with DirectShow (simplified)
    hr = RegisterFilter();
    
    CoUninitialize();
    return hr;
}

// DllUnregisterServer - Unregister the filter
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer()
{
    HRESULT hr = S_OK;
    
    // Initialize COM
    hr = CoInitialize(nullptr);
    if (FAILED(hr)) {
        return hr;
    }
    
    // Unregister filter
    hr = UnregisterFilter();
    
    // Unregister COM class from registry
    WCHAR clsidStr[64];
    StringFromGUID2(CLSID_TardsplayaDiscontinuityFilter, clsidStr, 64);
    
    WCHAR keyPath[256];
    swprintf_s(keyPath, L"CLSID\\%s", clsidStr);
    
    // Delete registry keys
    SHDeleteKey(HKEY_CLASSES_ROOT, keyPath);
    
    CoUninitialize();
    return hr;
}

} // extern "C"

// Helper Functions
void LockServer(BOOL bLock)
{
    if (bLock) {
        InterlockedIncrement(&g_cServerLocks);
    } else {
        InterlockedDecrement(&g_cServerLocks);
    }
}

HMODULE GetModuleHandle()
{
    return g_hInst;
}

BOOL IsFilterAvailable()
{
    return IsFilterRegistered();
}