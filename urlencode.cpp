#include "urlencode.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

std::wstring UrlEncode(const std::wstring& url) {
    wchar_t buf[4096];
    DWORD buflen = 4096;
    if (InternetCanonicalizeUrlW(url.c_str(), buf, &buflen, ICU_ENCODE_SPACES_ONLY | ICU_BROWSER_MODE))
        return buf;
    return url;
}