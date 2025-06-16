#pragma once
#include <string>

// URL encode a std::wstring using InternetCanonicalizeUrlW (WinINet)
std::wstring UrlEncode(const std::wstring& url);