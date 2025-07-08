#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include "twitch_api.h"
#include "urlencode.h"
#include "tlsclient/tlsclient.h"

// Helper: HTTP GET request (using WinHTTP, wide string version)
bool HttpGetText(const std::wstring& url, std::string& out) {
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        // Try TLS client as fallback if URL parsing fails
        return TLSClientHTTP::HttpGetText(url, out);
    }

    HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
    if (!hSession) {
        // Try TLS client as fallback
        return TLSClientHTTP::HttpGetText(url, out);
    }
    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { 
        WinHttpCloseHandle(hSession); 
        // Try TLS client as fallback
        return TLSClientHTTP::HttpGetText(url, out);
    }
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    BOOL res = WinHttpSendRequest(hRequest, 0, 0, 0, 0, 0, 0) && WinHttpReceiveResponse(hRequest, 0);
    if (!res) { 
        WinHttpCloseHandle(hRequest); 
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession); 
        // Try TLS client as fallback
        return TLSClientHTTP::HttpGetText(url, out);
    }

    DWORD dwSize = 0;
    std::vector<char> data;
    do {
        DWORD dwDownloaded = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (!dwSize) break;
        size_t prev_size = data.size();
        data.resize(prev_size + dwSize);
        WinHttpReadData(hRequest, data.data() + prev_size, dwSize, &dwDownloaded);
        if (dwDownloaded < dwSize) data.resize(prev_size + dwDownloaded);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);

    out.assign(data.begin(), data.end());
    return true;
}

// Lowercase helper
std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return towlower(c); });
    return out;
}

// Twitch API: Get stream qualities (simplified, no OAuth)
bool FetchTwitchStreamQualities(const std::wstring& channel, std::vector<std::wstring>& qualities, std::wstring& playlist_url, std::vector<std::wstring>* api_log) {
    // Channel must be lower-case
    std::wstring chan = ToLower(channel);
    std::wstring url = L"https://usher.ttvnw.net/api/channel/hls/" + chan + L".m3u8";
    url = UrlEncode(url);

    if (api_log) {
        api_log->push_back(L"Requesting Twitch HLS API:");
        api_log->push_back(url);
    }

    std::string playlist;
    if (!HttpGetText(url, playlist)) {
        if (api_log) api_log->push_back(L"Failed to get playlist.");
        return false;
    }

    // Parse available qualities from the playlist (look for #EXT-X-STREAM-INF:...NAME="source", etc.)
    std::istringstream ss(playlist);
    std::string line;
    qualities.clear();
    while (std::getline(ss, line)) {
        size_t pos = line.find("NAME=\"");
        if (pos != std::string::npos) {
            size_t end = line.find("\"", pos + 6);
            if (end != std::string::npos) {
                std::string q = line.substr(pos + 6, end - (pos + 6));
                std::wstring wq(q.begin(), q.end());
                qualities.push_back(wq);
            }
        }
    }
    playlist_url = url;
    return !qualities.empty();
}