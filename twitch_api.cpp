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
#include "json_minimal.h"

// Forward declaration - AddLog is defined in Tardsplaya.cpp
extern void AddLog(const std::wstring& msg);

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

// Modern GraphQL API approach for getting stream access token - based on TLS client example
std::wstring GetModernAccessToken(const std::wstring& channel) {
    // Convert channel to UTF-8 for the JSON request
    std::string channelUtf8;
    int len = WideCharToMultiByte(CP_UTF8, 0, channel.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        channelUtf8.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, channel.c_str(), -1, &channelUtf8[0], len, nullptr, nullptr);
    }
    
    // Try full GraphQL query instead of persisted query to avoid outdated hash issues
    std::string gqlBody = 
        "{"
        "\"query\":\"query PlaybackAccessToken($login: String!, $isLive: Boolean!, $vodID: ID!, $isVod: Boolean!, $playerType: String!) { streamPlaybackAccessToken(channelName: $login, params: {platform: \\\"web\\\", playerBackend: \\\"mediaplayer\\\", playerType: $playerType}) @include(if: $isLive) { value signature __typename } videoPlaybackAccessToken(id: $vodID, params: {platform: \\\"web\\\", playerBackend: \\\"mediaplayer\\\", playerType: $playerType}) @include(if: $isVod) { value signature __typename } }\","
        "\"variables\":{\"isLive\":true,\"login\":\"" + channelUtf8 + "\",\"isVod\":false,\"vodID\":\"\",\"playerType\":\"site\"}"
        "}";

    // Build headers
    std::wstring headers = 
        L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko\r\n"
        L"User-Agent: Mozilla/5.0\r\n"
        L"Content-Type: application/json\r\n";

    // Use TLS client to make the GraphQL POST request
    AddLog(L"Making GraphQL POST request to gql.twitch.tv for channel: " + channel);
    
    TLSClient client;
    std::string response;
    std::wstring url = L"https://gql.twitch.tv/gql";
    bool success = client.HttpPostW(url, gqlBody, response, headers);
    
    if (!success || response.empty()) {
        std::string error = client.GetLastError();
        std::wstring werror(error.begin(), error.end());
        AddLog(L"GraphQL POST request failed - " + werror);
        return L""; // Request failed
    }
    
    AddLog(L"GraphQL request completed, checking response...");
    
    // Log response details for debugging
    std::string body = get_http_body(response);
    if (body.empty()) {
        AddLog(L"Response received but body is empty after parsing");
        // Log the raw response for debugging
        std::wstring rawResponse(response.begin(), response.end());
        AddLog(L"Raw response: " + rawResponse.substr(0, 500)); // First 500 chars
        return L"";
    }
    
    AddLog(L"GraphQL response received, parsing JSON...");
    
    // Log the first 1000 characters of the response for debugging
    std::wstring debugBody(body.begin(), body.end());
    if (debugBody.length() > 1000) {
        debugBody = debugBody.substr(0, 1000) + L"...";
    }
    AddLog(L"GraphQL response body: " + debugBody);
    
    // Try to parse the JSON response
    try {
        JsonValue root = parse_json(body);
        if (root.type == JsonValue::Object) {
            // Check for errors first
            JsonValue errors = root["errors"];
            if (errors.type == JsonValue::Array) {
                AddLog(L"GraphQL response contains errors");
                return L""; // GraphQL errors present
            }
            
            JsonValue data = root["data"];
            if (data.type == JsonValue::Object) {
                JsonValue token_obj = data["streamPlaybackAccessToken"];
                if (token_obj.type == JsonValue::Object) {
                    std::string sig = token_obj["signature"].as_str();
                    std::string token = token_obj["value"].as_str();
                    
                    if (!sig.empty() && !token.empty()) {
                        AddLog(L"Successfully extracted token and signature from GraphQL response");
                        // Convert to wide strings and return in format expected by existing code
                        std::wstring wsig(sig.begin(), sig.end());
                        std::wstring wtoken(token.begin(), token.end());
                        return wsig + L"|" + wtoken;
                    } else {
                        AddLog(L"GraphQL response missing signature or token value");
                    }
                } else if (token_obj.type == JsonValue::Null) {
                    AddLog(L"Channel '" + channel + L"' is offline or does not exist");
                    return L"OFFLINE"; // Special return value to indicate channel is offline
                } else {
                    AddLog(L"GraphQL response missing streamPlaybackAccessToken object");
                }
            } else if (data.type == JsonValue::Null) {
                AddLog(L"GraphQL response data is null");
            } else {
                AddLog(L"GraphQL response missing data object");
            }
        } else {
            AddLog(L"GraphQL response is not a valid JSON object");
        }
    } catch (...) {
        AddLog(L"Exception occurred while parsing GraphQL JSON response");
        // JSON parsing failed, fall back to legacy API
    }
    
    return L""; // Failed to get token from GraphQL API
}

// Parse M3U8 playlist using improved logic from TLS client example
std::map<std::wstring, std::wstring> ParseM3U8Playlist(const std::string& m3u8Content) {
    std::map<std::wstring, std::wstring> result;
    std::istringstream iss(m3u8Content);
    std::string line, lastInfoLine;
    
    while (std::getline(iss, line)) {
        // Remove carriage return if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        if (line.find("#EXT-X-STREAM-INF:") == 0) {
            lastInfoLine = line;
            continue;
        }
        
        // If this is a URL line and we have a previous info line
        if (!line.empty() && line[0] != '#' && !lastInfoLine.empty()) {
            std::string quality = "Unknown";
            
            // Extract resolution from the info line
            auto resPos = lastInfoLine.find("RESOLUTION=");
            if (resPos != std::string::npos) {
                auto resStart = resPos + 11;
                auto resEnd = lastInfoLine.find(",", resStart);
                if (resEnd == std::string::npos) {
                    resEnd = lastInfoLine.find(" ", resStart);
                }
                if (resEnd == std::string::npos) {
                    resEnd = lastInfoLine.length();
                }
                quality = lastInfoLine.substr(resStart, resEnd - resStart);
            }
            
            // Extract NAME if available (for named qualities like "source")
            auto namePos = lastInfoLine.find("NAME=\"");
            if (namePos != std::string::npos) {
                auto nameStart = namePos + 6;
                auto nameEnd = lastInfoLine.find("\"", nameStart);
                if (nameEnd != std::string::npos) {
                    quality = lastInfoLine.substr(nameStart, nameEnd - nameStart);
                }
            }
            
            // Convert to wide strings
            std::wstring wQuality(quality.begin(), quality.end());
            std::wstring wUrl(line.begin(), line.end());
            result[wQuality] = wUrl;
            
            lastInfoLine.clear();
        }
    }
    
    return result;
}