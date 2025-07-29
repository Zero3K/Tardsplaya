#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <regex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <future>
#include "resource.h"
#include "json_minimal.h"
#include "stream_thread.h"
#include "tlsclient/tlsclient.h"
#include "twitch_api.h"
#include "favorites.h"
#include "playlist_parser.h"
#include "tsduck_transport_router.h"
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

struct StreamTab {
    std::wstring channel;
    HWND hChild;
    HWND hQualities;
    HWND hWatchBtn;
    HWND hStopBtn;
    std::vector<std::wstring> qualities;
    std::map<std::wstring, std::wstring> qualityToUrl;
    std::map<std::wstring, std::wstring> standardToOriginalQuality;
    std::thread streamThread;
    std::atomic<bool> cancelToken{false};
    std::atomic<bool> userRequestedStop{false}; // Track if user explicitly requested stop
    bool isStreaming = false;
    bool playerStarted = false; // Track if player has started successfully
    HANDLE playerProcess = nullptr; // Store player process handle for cleanup
    std::atomic<int> chunkCount{0}; // Track actual chunk queue size

    // Make the struct movable but not copyable
    StreamTab() : hChild(nullptr), hQualities(nullptr), hWatchBtn(nullptr), hStopBtn(nullptr) {};
    StreamTab(const StreamTab&) = delete;
    StreamTab& operator=(const StreamTab&) = delete;
    StreamTab(StreamTab&& other) noexcept 
        : channel(std::move(other.channel))
        , hChild(other.hChild)
        , hQualities(other.hQualities)
        , hWatchBtn(other.hWatchBtn)
        , hStopBtn(other.hStopBtn)
        , qualities(std::move(other.qualities))
        , qualityToUrl(std::move(other.qualityToUrl))
        , standardToOriginalQuality(std::move(other.standardToOriginalQuality))
        , streamThread(std::move(other.streamThread))
        , cancelToken(other.cancelToken.load())  // Preserve cancel state (moves should not happen with reserved capacity)
        , userRequestedStop(other.userRequestedStop.load())  // Preserve user stop state
        , isStreaming(other.isStreaming)
        , playerStarted(other.playerStarted)
        , playerProcess(other.playerProcess)
        , chunkCount(other.chunkCount.load())
    {
        // Note: With vector capacity reservation, moves should not happen during normal operation
        // This move constructor exists for completeness but should not be called for active streams
        std::wstring debug_msg = L"StreamTab move constructor called for channel: " + channel + 
                                L", cancelToken=" + std::to_wstring(cancelToken.load()) + 
                                L" (WARNING: move during active streaming should not happen with reserved capacity)";
        OutputDebugStringW(debug_msg.c_str());
        
        other.hChild = nullptr;
        other.hQualities = nullptr;
        other.hWatchBtn = nullptr;
        other.hStopBtn = nullptr;
        other.isStreaming = false;
        other.playerProcess = nullptr;
    }
    StreamTab& operator=(StreamTab&& other) noexcept {
        if (this != &other) {
            // Debug: Log move assignment usage
            std::wstring debug_msg = L"StreamTab move assignment called from " + other.channel + 
                                    L" to " + channel + L", cancelToken=" + 
                                    std::to_wstring(other.cancelToken.load()) + L" (WARNING: should not happen with reserved capacity)";
            OutputDebugStringW(debug_msg.c_str());
            
            channel = std::move(other.channel);
            hChild = other.hChild;
            hQualities = other.hQualities;
            hWatchBtn = other.hWatchBtn;
            hStopBtn = other.hStopBtn;
            qualities = std::move(other.qualities);
            qualityToUrl = std::move(other.qualityToUrl);
            standardToOriginalQuality = std::move(other.standardToOriginalQuality);
            streamThread = std::move(other.streamThread);
            cancelToken = other.cancelToken.load();  // Preserve cancel state (moves should not happen)
            userRequestedStop = other.userRequestedStop.load();  // Preserve user stop state
            isStreaming = other.isStreaming;
            playerStarted = other.playerStarted;
            playerProcess = other.playerProcess;
            chunkCount = other.chunkCount.load();
            
            other.hChild = nullptr;
            other.hQualities = nullptr;
            other.hWatchBtn = nullptr;
            other.hStopBtn = nullptr;
            other.isStreaming = false;
            other.playerStarted = false;
            other.playerProcess = nullptr;
        }
        return *this;
    }
};

HINSTANCE g_hInst;
HWND g_hMainWnd, g_hTab, g_hLogList, g_hStatusBar;
HWND g_hFavoritesList, g_hFavoritesAdd, g_hFavoritesDelete, g_hFavoritesEdit, g_hCheckVersion;
HFONT g_hFont = nullptr; // Tahoma font for UI controls
HACCEL g_hAccel = nullptr; // Accelerator table for hotkeys
std::vector<StreamTab> g_streams;
std::vector<std::wstring> g_favorites;

std::wstring g_playerPath = L"mpv.exe";
std::wstring g_playerArg = L"-";
bool g_enableLogging = true;
bool g_verboseDebug = false; // Enable verbose debug output for troubleshooting
bool g_logAutoScroll = true;
bool g_minimizeToTray = false;
bool g_logToFile = false; // Enable logging to debug.log file



// Tray icon support
NOTIFYICONDATA g_nid = {};
bool g_trayIconCreated = false;

void CreateTrayIcon() {
    if (g_trayIconCreated) return;
    
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = ID_TRAYICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(g_hInst, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Tardsplaya");
    
    Shell_NotifyIcon(NIM_ADD, &g_nid);
    g_trayIconCreated = true;
}

void RemoveTrayIcon() {
    if (!g_trayIconCreated) return;
    
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    g_trayIconCreated = false;
}

void ShowFromTray() {
    ShowWindow(g_hMainWnd, SW_RESTORE);
    SetForegroundWindow(g_hMainWnd);
    RemoveTrayIcon();
}

// Settings INI file functions
void LoadSettings() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring iniPath = exePath;
    size_t lastSlash = iniPath.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos) {
        iniPath = iniPath.substr(0, lastSlash + 1) + L"Tardsplaya.ini";
    } else {
        iniPath = L"Tardsplaya.ini";
    }

    wchar_t buffer[MAX_PATH];
    
    // Load player path
    GetPrivateProfileStringW(L"Settings", L"PlayerPath", L"mpv.exe", buffer, MAX_PATH, iniPath.c_str());
    g_playerPath = buffer;
    
    // Load player arguments
    GetPrivateProfileStringW(L"Settings", L"PlayerArgs", L"-", buffer, MAX_PATH, iniPath.c_str());
    g_playerArg = buffer;
    
    // Load minimize to tray setting
    g_minimizeToTray = GetPrivateProfileIntW(L"Settings", L"MinimizeToTray", 0, iniPath.c_str()) != 0;
    
    // Load file logging setting
    g_logToFile = GetPrivateProfileIntW(L"Settings", L"LogToFile", 0, iniPath.c_str()) != 0;
    
    // Load verbose debug setting
    g_verboseDebug = GetPrivateProfileIntW(L"Settings", L"VerboseDebug", 0, iniPath.c_str()) != 0;
}

void SaveSettings() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring iniPath = exePath;
    size_t lastSlash = iniPath.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos) {
        iniPath = iniPath.substr(0, lastSlash + 1) + L"Tardsplaya.ini";
    } else {
        iniPath = L"Tardsplaya.ini";
    }

    // Save player path
    WritePrivateProfileStringW(L"Settings", L"PlayerPath", g_playerPath.c_str(), iniPath.c_str());
    
    // Save player arguments
    WritePrivateProfileStringW(L"Settings", L"PlayerArgs", g_playerArg.c_str(), iniPath.c_str());
    
    // Save minimize to tray setting
    WritePrivateProfileStringW(L"Settings", L"MinimizeToTray", g_minimizeToTray ? L"1" : L"0", iniPath.c_str());
    
    // Save file logging setting
    WritePrivateProfileStringW(L"Settings", L"LogToFile", g_logToFile ? L"1" : L"0", iniPath.c_str());
    
    // Save verbose debug setting
    WritePrivateProfileStringW(L"Settings", L"VerboseDebug", g_verboseDebug ? L"1" : L"0", iniPath.c_str());
}

void AddLog(const std::wstring& msg) {
    if (!g_enableLogging) return;
    
    // Get timestamp for both UI and file logging
    wchar_t timebuf[32];
    time_t now = time(0);
    struct tm tmval;
    localtime_s(&tmval, &now);
    wcsftime(timebuf, 32, L"%H:%M:%S", &tmval);
    
    // Add to UI log list
    LVITEM item = { 0 };
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_hLogList);
    item.pszText = timebuf;
    ListView_InsertItem(g_hLogList, &item);
    ListView_SetItemText(g_hLogList, item.iItem, 1, const_cast<LPWSTR>(msg.c_str()));
    if (g_logAutoScroll) ListView_EnsureVisible(g_hLogList, item.iItem, FALSE);
    
    // Write to file if enabled
    if (g_logToFile) {
        FILE* logFile = nullptr;
        if (_wfopen_s(&logFile, L"debug.log", L"a") == 0 && logFile) {
            // Get full timestamp for file
            wchar_t fullTimeBuf[64];
            wcsftime(fullTimeBuf, 64, L"%Y-%m-%d %H:%M:%S", &tmval);
            fwprintf(logFile, L"[%s] %s\n", fullTimeBuf, msg.c_str());
            fclose(logFile);
        }
    }
}

// Add verbose debug logging function
void AddDebugLog(const std::wstring& msg) {
    if (!g_verboseDebug) return;
    AddLog(L"[DEBUG] " + msg);
}

void LoadFavorites() {
    g_favorites = LoadFavoritesFromFile(L"favorites.txt");
    SendMessage(g_hFavoritesList, LB_RESETCONTENT, 0, 0);
    for (const auto& fav : g_favorites) {
        SendMessage(g_hFavoritesList, LB_ADDSTRING, 0, (LPARAM)fav.c_str());
    }
}

void SaveFavorites() {
    SaveFavoritesToFile(L"favorites.txt", g_favorites);
}

void RefreshFavoritesList() {
    SendMessage(g_hFavoritesList, LB_RESETCONTENT, 0, 0);
    for (const auto& fav : g_favorites) {
        SendMessage(g_hFavoritesList, LB_ADDSTRING, 0, (LPARAM)fav.c_str());
    }
}

void AddFavorite() {
    // Get current channel name from active tab
    int activeTab = TabCtrl_GetCurSel(g_hTab);
    if (activeTab < 0 || activeTab >= (int)g_streams.size()) return;
    
    wchar_t channel[128];
    GetDlgItemText(g_streams[activeTab].hChild, IDC_CHANNEL, channel, 128);
    
    if (wcslen(channel) == 0) {
        MessageBoxW(g_hMainWnd, L"Enter a channel name first.", L"Add Favorite", MB_OK);
        return;
    }
    
    std::wstring channelStr = channel;
    // Check if already in favorites
    for (const auto& fav : g_favorites) {
        if (fav == channelStr) {
            MessageBoxW(g_hMainWnd, L"Channel is already in favorites.", L"Add Favorite", MB_OK);
            return;
        }
    }
    
    g_favorites.push_back(channelStr);
    RefreshFavoritesList();
    SaveFavorites();
}

void DeleteFavorite() {
    int sel = (int)SendMessage(g_hFavoritesList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxW(g_hMainWnd, L"Select a favorite to delete.", L"Delete Favorite", MB_OK);
        return;
    }
    
    g_favorites.erase(g_favorites.begin() + sel);
    RefreshFavoritesList();
    SaveFavorites();
}

void EditFavorite() {
    int sel = (int)SendMessage(g_hFavoritesList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxW(g_hMainWnd, L"Select a favorite to edit.", L"Edit Favorite", MB_OK);
        return;
    }
    
    // Simple edit dialog - for now just show the current name
    std::wstring currentName = g_favorites[sel];
    std::wstring message = L"Current favorite: " + currentName;
    MessageBoxW(g_hMainWnd, message.c_str(), L"Edit Favorite", MB_OK);
}

void UpdateAddFavoriteButtonState() {
    // Get current channel name from active tab
    int activeTab = TabCtrl_GetCurSel(g_hTab);
    if (activeTab < 0 || activeTab >= (int)g_streams.size()) {
        EnableWindow(g_hFavoritesAdd, FALSE);
        return;
    }
    
    wchar_t channel[128];
    GetDlgItemText(g_streams[activeTab].hChild, IDC_CHANNEL, channel, 128);
    
    // Enable/disable Add button based on whether channel text is empty
    EnableWindow(g_hFavoritesAdd, wcslen(channel) > 0);
}

void OnFavoriteDoubleClick() {
    int sel = (int)SendMessage(g_hFavoritesList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    
    // Load the favorite into the current tab
    int activeTab = TabCtrl_GetCurSel(g_hTab);
    if (activeTab < 0 || activeTab >= (int)g_streams.size()) return;
    
    HWND hChannelEdit = GetDlgItem(g_streams[activeTab].hChild, IDC_CHANNEL);
    if (hChannelEdit) {
        // Get the favorite text
        std::wstring favoriteText = g_favorites[sel];
        
        // Clear the existing text first
        SetWindowTextW(hChannelEdit, L"");
        
        // Set focus to ensure the control is active
        SetFocus(hChannelEdit);
        
        // Use SendMessage to set text more reliably
        SendMessageW(hChannelEdit, WM_SETTEXT, 0, (LPARAM)favoriteText.c_str());
        
        // Force the control to process the message
        UpdateWindow(hChannelEdit);
        
        // Verify the text was set correctly
        wchar_t verify[128];
        GetWindowTextW(hChannelEdit, verify, 128);
        
        // If verification failed, try alternative method
        if (wcscmp(verify, favoriteText.c_str()) != 0) {
            // Alternative: simulate typing the text
            SendMessageW(hChannelEdit, EM_SETSEL, 0, -1); // Select all text
            SendMessageW(hChannelEdit, EM_REPLACESEL, TRUE, (LPARAM)favoriteText.c_str());
        }
        
        // Send a change notification to trigger any dependent logic
        SendMessage(GetParent(hChannelEdit), WM_COMMAND, MAKEWPARAM(IDC_CHANNEL, EN_CHANGE), (LPARAM)hChannelEdit);
    }
    UpdateAddFavoriteButtonState();
}

void CheckVersion() {
    MessageBoxW(g_hMainWnd, L"Tardsplaya Version 1.0\nTwitch Stream Player", L"Version", MB_OK);
}

void UpdateStatusBar(const std::wstring& text) {
    if (g_hStatusBar) {
        SetWindowTextW(g_hStatusBar, text.c_str());
    }
}

std::string WideToUtf8(const std::wstring& w) {
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return std::string();
    std::string out(static_cast<size_t>(sz - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], sz, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (sz <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(sz - 1), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], sz);
    return out;
}

std::string HttpGet(const wchar_t* host, const wchar_t* path, const wchar_t* headers = nullptr) {
    HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        // Fallback to TLS client if WinHTTP fails to initialize
        std::string result = TLSClientHTTP::HttpGet(host, path, headers ? headers : L"");
        if (!result.empty()) return result;
        return "";
    }
    
    // For Windows 7 compatibility - disable certificate validation if needed
    DWORD dwFlags = WINHTTP_FLAG_SECURE;
    
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { 
        WinHttpCloseHandle(hSession); 
        // Fallback to TLS client
        std::string result = TLSClientHTTP::HttpGet(host, path, headers ? headers : L"");
        if (!result.empty()) return result;
        return ""; 
    }
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) { 
        WinHttpCloseHandle(hConnect); 
        WinHttpCloseHandle(hSession); 
        // Fallback to TLS client
        std::string result = TLSClientHTTP::HttpGet(host, path, headers ? headers : L"");
        if (!result.empty()) return result;
        return ""; 
    }
    
    // For Windows 7 compatibility - ignore certificate errors
    DWORD dwSecurityFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                           SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                           SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                           SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags));
    
    BOOL bResult = WinHttpSendRequest(hRequest, headers, headers ? -1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL);
    std::string data;
    if (bResult) {
        DWORD dwSize = 0;
        do {
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            std::vector<char> buf(dwSize);
            WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded);
            data.append(buf.data(), dwDownloaded);
        } while (dwSize > 0);
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    // If WinHTTP didn't return data, try TLS client as fallback
    if (data.empty()) {
        std::string result = TLSClientHTTP::HttpGet(host, path, headers ? headers : L"");
        if (!result.empty()) return result;
    }
    
    return data;
}

std::wstring GetAccessToken(const std::wstring& channel) {
    // First try the modern GraphQL API approach
    AddLog(L"Trying modern GraphQL API...");
    std::wstring modernToken = GetModernAccessToken(channel);
    if (!modernToken.empty()) {
        if (modernToken == L"OFFLINE") {
            AddLog(L"Channel is offline or does not exist - skipping legacy API attempts");
            return L""; // Don't try legacy API for offline channels
        }
        AddLog(L"Modern GraphQL API succeeded");
        return modernToken;
    }
    AddLog(L"Modern GraphQL API failed, trying legacy API...");
    
    // Fallback to legacy API approach
    std::wstring path = L"/api/channels/" + channel + L"/access_token?need_https=true&oauth_token=&platform=web&player_backend=mediaplayer&player_type=site";
    AddLog(L"Trying gql.twitch.tv endpoint...");
    std::string resp = HttpGet(L"gql.twitch.tv", path.c_str(), L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko");
    
    // Fallback to alternative endpoint if first fails
    if (resp.empty()) {
        AddLog(L"gql.twitch.tv failed, trying api.twitch.tv endpoint...");
        path = L"/api/channels/" + channel + L"/access_token";
        resp = HttpGet(L"api.twitch.tv", path.c_str(), L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko");
    }
    
    if (resp.empty()) {
        AddLog(L"All HTTP requests failed - no response received");
        return L"";
    }
    
    AddLog(L"Parsing JSON response...");
    JsonValue jv = parse_json(resp);
    std::string token, sig;
    if (jv.type == JsonValue::Type::Object) {
        token = jv["token"].as_str();
        sig = jv["sig"].as_str();
        if (!token.empty() && !sig.empty()) {
            AddLog(L"Successfully parsed token and signature from legacy API");
        } else {
            AddLog(L"JSON response missing token or signature fields");
        }
    } else {
        AddLog(L"JSON parsing failed - invalid response format");
    }
    
    if (token.empty() || sig.empty()) return L"";
    std::wstring wsig = Utf8ToWide(sig);
    std::wstring wtoken = Utf8ToWide(token);
    return wsig + L"|" + wtoken;
}

std::wstring FetchPlaylist(const std::wstring& channel, const std::wstring& accessToken) {
    size_t sep = accessToken.find(L'|');
    if (sep == std::wstring::npos) return L"";
    std::wstring sig = accessToken.substr(0, sep);
    std::wstring token = accessToken.substr(sep + 1);
    
    // URL encode the token and sig for safety
    std::string tokenUtf8 = WideToUtf8(token);
    std::string sigUtf8 = WideToUtf8(sig);
    
    // Build the playlist URL with proper parameters
    std::wstring path = L"/api/channel/hls/" + channel +
        L".m3u8?player=twitchweb&allow_source=true&allow_audio_only=true&type=any&p=" +
        std::to_wstring(rand() % 999999) + L"&token=" + token + L"&sig=" + sig;
    
    std::string resp = HttpGet(L"usher.ttvnw.net", path.c_str());
    return Utf8ToWide(resp);
}

std::map<std::wstring, std::wstring> ParsePlaylist(const std::wstring& m3u8) {
    // First try the improved M3U8 parser
    std::vector<PlaylistQuality> modernResult = ParseM3U8MasterPlaylist(m3u8);
    if (!modernResult.empty()) {
        std::map<std::wstring, std::wstring> qualityMap;
        for (const auto& quality : modernResult) {
            qualityMap[quality.name] = quality.url;
        }
        return qualityMap;
    }
    
    // Fallback to original parser for backwards compatibility
    std::map<std::wstring, std::wstring> result;
    std::wistringstream iss(m3u8);
    std::wstring line, quality, url;
    while (std::getline(iss, line)) {
        if (line.find(L"#EXT-X-STREAM-INF:") == 0) {
            std::wsmatch m;
            std::wregex rgx(L"VIDEO=\"([^\"]+)\"");
            if (std::regex_search(line, m, rgx))
                quality = m[1];
            else {
                // Try to extract resolution or other quality indicators
                std::wregex resRgx(L"RESOLUTION=([0-9]+x[0-9]+)");
                if (std::regex_search(line, m, resRgx)) {
                    quality = m[1];
                } else {
                    quality = L"unknown";
                }
            }
            std::getline(iss, url);
            if (!url.empty() && url[0] != L'#') {
                result[quality] = url;
            }
        }
    }
    
    // If no stream-inf found, try to find any stream URLs for non-chunked streams
    if (result.empty() && m3u8.find(L"#EXTM3U") == 0) {
        std::wistringstream iss2(m3u8);
        while (std::getline(iss2, url)) {
            if (!url.empty() && url[0] != L'#' &&
                (url.find(L".m3u8") != std::wstring::npos || 
                 url.find(L".ts") != std::wstring::npos ||
                 url.find(L"http") == 0)) {
                result[L"source"] = url;
                break;
            }
        }
    }
    return result;
}

std::wstring StandardizeQualityName(const std::wstring& originalName) {
    // Map common quality patterns to Streamlink format
    std::wstring lower = originalName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    
    if (lower.find(L"audio") != std::wstring::npos || lower == L"unknown") {
        return L"audio_only";
    }
    if (lower.find(L"1080p60") != std::wstring::npos || lower.find(L"1080p_60") != std::wstring::npos) {
        return L"1080p60";
    }
    if (lower.find(L"720p60") != std::wstring::npos || lower.find(L"720p_60") != std::wstring::npos) {
        return L"720p60";
    }
    if (lower.find(L"720p") != std::wstring::npos) {
        return L"720p";
    }
    if (lower.find(L"480p") != std::wstring::npos) {
        return L"480p";
    }
    if (lower.find(L"360p") != std::wstring::npos) {
        return L"360p";
    }
    if (lower.find(L"160p") != std::wstring::npos) {
        return L"160p";
    }
    
    // Handle resolution strings like "1920x1080", "1280x720", etc.
    std::wregex resolutionPattern(L"(\\d+)x(\\d+)");
    std::wsmatch resMatch;
    if (std::regex_search(originalName, resMatch, resolutionPattern)) {
        int width = std::stoi(resMatch[1].str());
        int height = std::stoi(resMatch[2].str());
        
        // Map common resolutions to quality names
        if (height >= 1080) return L"1080p60"; // Assume high res is 60fps
        if (height >= 720) return L"720p";
        if (height >= 480) return L"480p";
        if (height >= 360) return L"360p";
        if (height >= 160) return L"160p";
        return L"audio_only"; // Very low resolution
    }
    
    // Try to extract resolution from string patterns like "1080p60", "720p", etc.
    std::wregex resPattern(L"(\\d+)p(?:(\\d+))?");
    std::wsmatch match;
    if (std::regex_search(originalName, match, resPattern)) {
        std::wstring res = match[1].str() + L"p";
        if (match[2].matched) {
            res += match[2].str();
        }
        return res;
    }
    
    // Default fallback
    return originalName.empty() ? L"audio_only" : originalName;
}

std::vector<std::wstring> SortQualities(const std::vector<std::wstring>& qualities) {
    // Define Streamlink quality order (highest to lowest)
    std::vector<std::wstring> order = {
        L"1080p60", L"720p60", L"720p", L"480p", L"360p", L"160p", L"audio_only"
    };
    
    std::vector<std::wstring> sorted;
    
    // Add qualities in the defined order if they exist
    for (const auto& preferredQuality : order) {
        for (const auto& quality : qualities) {
            if (StandardizeQualityName(quality) == preferredQuality) {
                sorted.push_back(preferredQuality);
                break;
            }
        }
    }
    
    // Add any remaining qualities that didn't match the standard format
    for (const auto& quality : qualities) {
        std::wstring standardized = StandardizeQualityName(quality);
        bool alreadyAdded = false;
        for (const auto& added : sorted) {
            if (added == standardized) {
                alreadyAdded = true;
                break;
            }
        }
        if (!alreadyAdded) {
            sorted.push_back(standardized);
        }
    }
    
    return sorted;
}

void RefreshQualities(StreamTab& tab) {
    SendMessage(tab.hQualities, LB_RESETCONTENT, 0, 0);
    tab.standardToOriginalQuality.clear();
    
    // Sort qualities in Streamlink format and build mapping
    std::vector<std::wstring> sortedQualities = SortQualities(tab.qualities);
    
    for (const auto& originalQuality : tab.qualities) {
        std::wstring standardized = StandardizeQualityName(originalQuality);
        tab.standardToOriginalQuality[standardized] = originalQuality;
    }
    
    for (const auto& q : sortedQualities)
        SendMessage(tab.hQualities, LB_ADDSTRING, 0, (LPARAM)q.c_str());
}

void InitLogList(HWND hList) {
    LVCOLUMN lvc = { 0 };
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.pszText = (LPWSTR)L"Time";
    lvc.cx = 90;
    ListView_InsertColumn(hList, 0, &lvc);
    lvc.pszText = (LPWSTR)L"Log";
    lvc.cx = 360;
    ListView_InsertColumn(hList, 1, &lvc);
}

void LoadChannel(StreamTab& tab) {
    wchar_t channel[128];
    int result = GetDlgItemText(tab.hChild, IDC_CHANNEL, channel, 128);
    
    // Add debugging info
    std::wstring debugMsg = L"Retrieved channel name: '" + std::wstring(channel) + L"' (length: " + std::to_wstring(wcslen(channel)) + L", GetDlgItemText result: " + std::to_wstring(result) + L")";
    AddLog(debugMsg);
    
    if (wcslen(channel) == 0) {
        MessageBoxW(tab.hChild, L"Enter a channel name.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Clean the channel name to remove any non-printable characters
    std::wstring channelStr = channel;
    std::wstring cleanedChannel;
    for (wchar_t c : channelStr) {
        // Only include printable ASCII characters, letters, digits, underscore
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_') {
            cleanedChannel += c;
        }
    }
    
    // If we removed characters, log it and use the cleaned version
    if (cleanedChannel.length() != channelStr.length()) {
        AddLog(L"Cleaned channel name from '" + channelStr + L"' to '" + cleanedChannel + L"'");
        channelStr = cleanedChannel;
        
        // Update the text box with the cleaned name
        SetDlgItemTextW(tab.hChild, IDC_CHANNEL, cleanedChannel.c_str());
    }
    
    if (channelStr.empty()) {
        MessageBoxW(tab.hChild, L"Enter a valid channel name.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    // Convert channel name to lowercase for API calls
    std::wstring channelNameLower = channelStr;
    std::transform(channelNameLower.begin(), channelNameLower.end(), channelNameLower.begin(), ::towlower);
    
    tab.channel = channelStr; // Store the cleaned version for display
    AddLog(L"Requesting Twitch access token for: " + channelNameLower);
    std::wstring token = GetAccessToken(channelNameLower);
    if (token.empty()) {
        MessageBoxW(tab.hChild, L"Failed to get access token. The channel may be offline, does not exist, or has been renamed.", L"Channel Error", MB_OK | MB_ICONERROR);
        AddLog(L"Failed to get Twitch access token - channel may be offline or not exist.");
        return;
    }
    AddLog(L"Fetching playlist...");
    std::wstring m3u8 = FetchPlaylist(channelNameLower, token);
    if (m3u8.empty()) {
        MessageBoxW(tab.hChild, L"Failed to get playlist. The channel may be offline, no longer exist, or have been renamed.", L"Channel Error", MB_OK | MB_ICONERROR);
        AddLog(L"Failed to get playlist - channel may be offline or not exist.");
        return;
    }
    AddLog(L"Parsing qualities...");
    tab.qualityToUrl = ParsePlaylist(m3u8);
    tab.qualities.clear();
    for (const auto& pair : tab.qualityToUrl)
        tab.qualities.push_back(pair.first);
    RefreshQualities(tab);
    if (tab.qualities.empty()) {
        MessageBoxW(tab.hChild, L"No stream qualities found. The stream may use unsupported encoding or be unavailable.", L"Stream Error", MB_OK | MB_ICONERROR);
        AddLog(L"No qualities found - stream may use unsupported encoding.");
        EnableWindow(tab.hWatchBtn, FALSE);
    }
    else {
        // Only enable the watch button if we're not currently streaming
        if (!tab.isStreaming) {
            EnableWindow(tab.hWatchBtn, TRUE);
        }
    }
}

void StopStream(StreamTab& tab, bool userInitiated = false) {
    AddDebugLog(L"StopStream: Starting for channel=" + tab.channel + 
               L", userInitiated=" + std::to_wstring(userInitiated) + 
               L", isStreaming=" + std::to_wstring(tab.isStreaming));
    
    if (tab.isStreaming) {
        AddDebugLog(L"StopStream: Setting cancel token for " + tab.channel);
        tab.cancelToken = true;
        if (userInitiated) {
            tab.userRequestedStop = true;
            AddDebugLog(L"StopStream: User requested stop set for " + tab.channel);
        }
        if (tab.streamThread.joinable()) {
            AddDebugLog(L"StopStream: Joining stream thread for " + tab.channel);
            tab.streamThread.join();
            AddDebugLog(L"StopStream: Stream thread joined for " + tab.channel);
        }
        tab.isStreaming = false;
        tab.playerStarted = false;
        
        EnableWindow(tab.hWatchBtn, TRUE);
        EnableWindow(tab.hStopBtn, FALSE);
        // Re-enable channel textbox, quality listbox, and Load button when streaming stops
        EnableWindow(GetDlgItem(tab.hChild, IDC_CHANNEL), TRUE);
        EnableWindow(tab.hQualities, TRUE);
        EnableWindow(GetDlgItem(tab.hChild, IDC_LOAD), TRUE);
        SetWindowTextW(tab.hWatchBtn, L"2. Watch");
        
        // Check if any other streams are still active
        bool hasActiveStream = false;
        for (const auto& otherTab : g_streams) {
            if (&otherTab != &tab && otherTab.isStreaming) {
                hasActiveStream = true;
                break;
            }
        }
        
        if (!hasActiveStream) {
            UpdateStatusBar(L"Buffer: 0 packets");
        }
        
        AddLog(L"Stream stopped.");
    }
}

void WatchStream(StreamTab& tab, size_t tabIndex) {
    AddDebugLog(L"WatchStream: Starting for tab " + std::to_wstring(tabIndex) + 
               L", channel=" + tab.channel + L", isStreaming=" + std::to_wstring(tab.isStreaming));
    
    if (tab.isStreaming) {
        AddDebugLog(L"WatchStream: Stream already running, stopping first for tab " + std::to_wstring(tabIndex));
        StopStream(tab, true); // User clicked watch to stop current stream
        return;
    }



    // Check if player path exists
    DWORD dwAttrib = GetFileAttributesW(g_playerPath.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(tab.hChild, L"Media player not found. Please check the player path in Settings.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    int sel = (int)SendMessage(tab.hQualities, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxW(tab.hChild, L"Select a quality.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t qual[64];
    SendMessage(tab.hQualities, LB_GETTEXT, sel, (LPARAM)qual);
    qual[63] = L'\0'; // Ensure null termination
    
    // Find the original quality name from the standardized name
    std::wstring standardQuality = qual;
    std::wstring originalQuality;
    auto mappingIt = tab.standardToOriginalQuality.find(standardQuality);
    if (mappingIt != tab.standardToOriginalQuality.end()) {
        originalQuality = mappingIt->second;
    } else {
        originalQuality = standardQuality; // fallback
    }
    
    auto it = tab.qualityToUrl.find(originalQuality);
    if (it == tab.qualityToUrl.end()) {
        MessageBoxW(tab.hChild, L"Failed to resolve quality URL.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    std::wstring url = it->second;
    AddLog(L"Starting buffered stream for " + tab.channel + L" (" + standardQuality + L") with Frame Number Tagging");
    
    // Log current stream status for debugging multi-stream issues
    int active_streams = 0;
    std::wstring active_channels;
    for (size_t i = 0; i < g_streams.size(); i++) {
        if (g_streams[i].isStreaming) {
            active_streams++;
            active_channels += L" [" + std::to_wstring(i) + L"]:" + g_streams[i].channel;
        }
    }
    AddDebugLog(L"WatchStream: Starting new stream " + tab.channel + L" when " + std::to_wstring(active_streams) + 
               L" streams already active:" + active_channels);
    
    // Add small delay between stream starts to reduce resource contention
    if (active_streams > 0) {
        AddDebugLog(L"WatchStream: Adding startup delay for multi-stream scenario");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 + active_streams * 500));
    }
    
    // Reset cancel token and user requested stop flag
    tab.cancelToken = false;
    tab.userRequestedStop = false;
    
    AddDebugLog(L"WatchStream: Creating stream thread for tab " + std::to_wstring(tabIndex) + 
               L", PlayerPath=" + g_playerPath + L", URL=" + url);
    
    // GPAC Decoder Mode is now the default streaming mode (replaces TSDuck)
    StreamingMode mode = StreamingMode::GPAC_DECODER;
    
    AddLog(L"[GPAC] Starting GPAC-based media decoding for " + tab.channel + L" (" + standardQuality + L")");
    AddLog(L"[GPAC] GPAC will decode to raw AVI/WAV and pipe to media player");
    
    // Start the buffering thread
    tab.streamThread = StartStreamThread(
        g_playerPath,
        url,
        tab.cancelToken,
        [](const std::wstring& msg) {
            // Log callback - post message to main thread for thread-safe logging
            PostMessage(g_hMainWnd, WM_USER + 1, 0, (LPARAM)new std::wstring(msg));
        },
        3, // buffer 3 segments (for HLS) or 3000 packets (for TS)
        tab.channel, // channel name for player window title
        &tab.chunkCount, // chunk count for status display
        &tab.userRequestedStop, // user requested stop flag
        g_hMainWnd, // main window handle for auto-stop messages
        tabIndex, // tab index for identifying which stream to auto-stop
        originalQuality, // selected quality for ad recovery
        mode, // streaming mode (HLS or Transport Stream)
        &tab.playerProcess // player process handle for monitoring
    );
    
    AddDebugLog(L"WatchStream: Stream thread created successfully for tab " + std::to_wstring(tabIndex));
    
    tab.isStreaming = true;
    tab.playerStarted = false;
    EnableWindow(tab.hWatchBtn, FALSE);
    EnableWindow(tab.hStopBtn, TRUE);
    // Disable channel textbox, quality listbox, and Load button when streaming starts
    EnableWindow(GetDlgItem(tab.hChild, IDC_CHANNEL), FALSE);
    EnableWindow(tab.hQualities, FALSE);
    EnableWindow(GetDlgItem(tab.hChild, IDC_LOAD), FALSE);
    SetWindowTextW(tab.hWatchBtn, L"Starting...");
    UpdateStatusBar(L"Buffer: Buffering... | GPAC Decoder Active");
    
    AddDebugLog(L"WatchStream: UI updated, stream starting for tab " + std::to_wstring(tabIndex));
    
    // Set a timer to update the button text after 3 seconds (player should be started by then)
    SetTimer(g_hMainWnd, TIMER_PLAYER_CHECK, 3000, nullptr);
    
    // Set a timer to periodically update chunk queue status
    SetTimer(g_hMainWnd, TIMER_CHUNK_UPDATE, 2000, nullptr);
    
    // Don't detach the thread - keep it joinable for proper synchronization
}

LRESULT CALLBACK StreamChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        // (Handled by parent)
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        // Handle Enter key in Channel text box
        if (wParam == VK_RETURN) {
            HWND hFocused = GetFocus();
            if (hFocused && GetDlgCtrlID(hFocused) == IDC_CHANNEL) {
                // Get tab index and trigger Load functionality
                int tabIndex = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
                if (tabIndex >= 0 && tabIndex < (int)g_streams.size()) {
                    StreamTab& tab = g_streams[tabIndex];
                    LoadChannel(tab);
                }
                return 0; // Prevent default processing
            }
        }
    }
    if (msg == WM_COMMAND) {
        // Get tab index instead of pointer to avoid vector reallocation issues
        int tabIndex = (int)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        if (tabIndex < 0 || tabIndex >= (int)g_streams.size()) return 0;
        StreamTab& tab = g_streams[tabIndex];
        
        switch (LOWORD(wParam)) {
        case IDC_LOAD:
            LoadChannel(tab);
            break;
        case IDC_WATCH:
            WatchStream(tab, tabIndex);
            break;
        case IDC_STOP:
            StopStream(tab, true); // User clicked stop button
            break;
        case IDC_CHANNEL:
            if (HIWORD(wParam) == EN_CHANGE) {
                UpdateAddFavoriteButtonState();
            }
            break;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

ATOM RegisterStreamChildClass() {
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = StreamChildProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"StreamChildWin";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    return RegisterClass(&wc);
}

HWND CreateStreamChild(HWND hParent, StreamTab& tab, const wchar_t* channel = L"") {
    RECT rc = { 0, 0, 480, 180 };
    HWND hwnd = CreateWindowEx(0, L"StreamChildWin", NULL,
        WS_CHILD | WS_VISIBLE,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hParent, nullptr, g_hInst, nullptr);
    
    HWND hChannelLabel = CreateWindowEx(0, L"STATIC", L"Channel:", WS_CHILD | WS_VISIBLE, 10, 10, 55, 18, hwnd, nullptr, g_hInst, nullptr);
    SendMessage(hChannelLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", channel, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 10, 200, 22, hwnd, (HMENU)IDC_CHANNEL, g_hInst, nullptr);
    SendMessage(hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hQualityLabel = CreateWindowEx(0, L"STATIC", L"Quality:", WS_CHILD | WS_VISIBLE, 10, 40, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    SendMessage(hQualityLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hQualList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", 0, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 70, 40, 200, 120, hwnd, (HMENU)IDC_QUALITIES, g_hInst, nullptr);
    SendMessage(hQualList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hLoad = CreateWindowEx(0, L"BUTTON", L"1. Load", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 40, 60, 22, hwnd, (HMENU)IDC_LOAD, g_hInst, nullptr);
    SendMessage(hLoad, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hWatch = CreateWindowEx(0, L"BUTTON", L"2. Watch", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 70, 60, 22, hwnd, (HMENU)IDC_WATCH, g_hInst, nullptr);
    SendMessage(hWatch, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    
    HWND hStop = CreateWindowEx(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 100, 60, 22, hwnd, (HMENU)IDC_STOP, g_hInst, nullptr);
    SendMessage(hStop, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    EnableWindow(hWatch, FALSE);
    EnableWindow(hStop, FALSE);
    


    tab.hChild = hwnd;
    tab.hQualities = hQualList;
    tab.hWatchBtn = hWatch;
    tab.hStopBtn = hStop;
    // Store index instead of pointer to avoid vector reallocation issues
    // We'll set this properly in AddStreamTab after the tab is added to vector
    return hwnd;
}

void ResizeTabAndChildren(HWND hwnd) {
    RECT rcMain, rcStatus;
    GetClientRect(hwnd, &rcMain);
    
    // Get status bar height
    GetWindowRect(g_hStatusBar, &rcStatus);
    int statusHeight = rcStatus.bottom - rcStatus.top;
    
    // Resize status bar
    SendMessage(g_hStatusBar, WM_SIZE, 0, 0);
    
    // Layout constants
    const int favoritesWidth = 200;
    const int margin = 10;
    const int logHeight = 120;
    
    // Calculate available area (minus status bar)
    int availableHeight = rcMain.bottom - statusHeight;
    
    // Position favorites panel
    SetWindowPos(g_hFavoritesList, nullptr, margin, 30, favoritesWidth - 20, availableHeight - 100, SWP_NOZORDER);
    SetWindowPos(g_hFavoritesAdd, nullptr, margin, availableHeight - 60, 40, 25, SWP_NOZORDER);
    SetWindowPos(g_hFavoritesDelete, nullptr, margin + 45, availableHeight - 60, 50, 25, SWP_NOZORDER);
    SetWindowPos(g_hFavoritesEdit, nullptr, margin + 100, availableHeight - 60, 40, 25, SWP_NOZORDER);
    SetWindowPos(g_hCheckVersion, nullptr, margin, availableHeight - 30, 100, 25, SWP_NOZORDER);
    
    // Position stream tab control (main area, excluding favorites panel)
    int mainAreaX = favoritesWidth + margin;
    int mainAreaWidth = rcMain.right - mainAreaX - margin;
    SetWindowPos(g_hTab, nullptr, mainAreaX, margin, mainAreaWidth, availableHeight - logHeight - margin, SWP_NOZORDER);
    
    // Position log list at the bottom
    SetWindowPos(g_hLogList, nullptr, mainAreaX, availableHeight - logHeight, mainAreaWidth, logHeight, SWP_NOZORDER);
    
    // Resize stream tab children
    int sel = TabCtrl_GetCurSel(g_hTab);
    for (size_t i = 0; i < g_streams.size(); ++i) {
        ShowWindow(g_streams[i].hChild, i == sel ? SW_SHOW : SW_HIDE);
        if (i == sel) {
            RECT rcTab;
            GetClientRect(g_hTab, &rcTab);
            TabCtrl_AdjustRect(g_hTab, FALSE, &rcTab);
            SetWindowPos(g_streams[i].hChild, nullptr, rcTab.left, rcTab.top, 
                        rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }
}

void AddStreamTab(const std::wstring& channel = L"") {
    TCITEM tie = { 0 };
    tie.mask = TCIF_TEXT;
    
    std::wstring tabName;
    if (channel.empty()) {
        // Generate numbered tab name: TP Stream 01, TP Stream 02, etc.
        int tabCount = TabCtrl_GetItemCount(g_hTab) + 1;
        wchar_t buffer[32];
        swprintf_s(buffer, L"TP Stream %02d", tabCount);
        tabName = buffer;
    } else {
        tabName = channel;
    }
    
    tie.pszText = (LPWSTR)tabName.c_str();
    int idx = TabCtrl_GetItemCount(g_hTab);
    TabCtrl_InsertItem(g_hTab, idx, &tie);
    
    // Create the tab in-place to avoid copying
    size_t old_capacity = g_streams.capacity();
    AddDebugLog(L"AddStreamTab: Before emplace_back - size=" + std::to_wstring(g_streams.size()) + 
               L", capacity=" + std::to_wstring(old_capacity) + L" for " + channel);
    g_streams.emplace_back();
    size_t new_capacity = g_streams.capacity();
    if (new_capacity != old_capacity) {
        AddDebugLog(L"AddStreamTab: *** CRITICAL *** VECTOR REALLOCATED - old_capacity=" + std::to_wstring(old_capacity) + 
                   L", new_capacity=" + std::to_wstring(new_capacity) + L", this will cause cancelToken use-after-free!");
    } else {
        AddDebugLog(L"AddStreamTab: No reallocation - capacity stable at " + std::to_wstring(new_capacity));
    }
    StreamTab& tab = g_streams.back();
    HWND hChild = CreateStreamChild(g_hTab, tab, channel.c_str());
    
    // Store the index instead of pointer to avoid vector reallocation issues
    SetWindowLongPtr(hChild, GWLP_USERDATA, (LONG_PTR)idx);
    
    TabCtrl_SetCurSel(g_hTab, idx);
    ResizeTabAndChildren(g_hMainWnd);
    UpdateAddFavoriteButtonState();
}

void SwitchToTab(int idx) {
    if (idx < 0 || idx >= (int)g_streams.size()) return;
    TabCtrl_SetCurSel(g_hTab, idx);
    ResizeTabAndChildren(g_hMainWnd);
    SetFocus(g_streams[idx].hChild);
    UpdateAddFavoriteButtonState();
}

void CloseActiveTab() {
    int cur = TabCtrl_GetCurSel(g_hTab);
    if (cur < 0 || cur >= (int)g_streams.size()) return;
    DestroyWindow(g_streams[cur].hChild);
    g_streams.erase(g_streams.begin() + cur);
    TabCtrl_DeleteItem(g_hTab, cur);
    
    // Update indices stored in remaining tabs
    for (int i = cur; i < (int)g_streams.size(); ++i) {
        SetWindowLongPtr(g_streams[i].hChild, GWLP_USERDATA, (LONG_PTR)i);
    }
    
    if (!g_streams.empty()) {
        int newIdx = cur < (int)g_streams.size() ? cur : (int)g_streams.size() - 1;
        SwitchToTab(newIdx);
    }
    ResizeTabAndChildren(g_hMainWnd);
}

void CloseAllTabs() {
    // Stop all streams before closing tabs
    for (auto& s : g_streams) {
        if (s.isStreaming) {
            s.cancelToken = true;
            s.userRequestedStop = true; // Ensure user stop is flagged for cleanup
        }
    }
    
    // Give threads time to respond to cancellation, but don't block indefinitely
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    
    // Use aggressive cleanup to prevent hanging indefinitely
    for (auto& s : g_streams) {
        if (s.streamThread.joinable()) {
            // Set a deadline for thread cleanup
            auto cleanup_start = std::chrono::steady_clock::now();
            const auto max_cleanup_time = std::chrono::seconds(3);
            
            // Try to join but don't wait forever
            try {
                // Use a future to make the join operation timeout-able
                auto join_future = std::async(std::launch::async, [&s]() {
                    if (s.streamThread.joinable()) {
                        s.streamThread.join();
                    }
                });
                
                // Wait for the join to complete or timeout
                auto status = join_future.wait_for(max_cleanup_time);
                if (status == std::future_status::timeout) {
                    // Thread didn't finish in time, detach it
                    if (s.streamThread.joinable()) {
                        s.streamThread.detach();
                    }
                }
            } catch (...) {
                // If anything goes wrong, detach the thread
                if (s.streamThread.joinable()) {
                    s.streamThread.detach();
                }
            }
        }
        
        // More robust media player process termination
        if (s.playerProcess && s.playerProcess != INVALID_HANDLE_VALUE) {
            // First try to close gracefully by sending WM_CLOSE to the process windows
            DWORD processId = GetProcessId(s.playerProcess);
            if (processId != 0) {
                // Enumerate windows of the process and send WM_CLOSE
                EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                    DWORD windowProcessId;
                    GetWindowThreadProcessId(hwnd, &windowProcessId);
                    if (windowProcessId == (DWORD)lParam) {
                        PostMessage(hwnd, WM_CLOSE, 0, 0);
                    }
                    return TRUE;
                }, (LPARAM)processId);
                
                // Wait a bit for graceful shutdown
                if (WaitForSingleObject(s.playerProcess, 2000) != WAIT_OBJECT_0) {
                    // If still running, force terminate
                    TerminateProcess(s.playerProcess, 0);
                    WaitForSingleObject(s.playerProcess, 1000);
                }
            }
            CloseHandle(s.playerProcess);
            s.playerProcess = nullptr;
        }
        
        if (s.hChild) DestroyWindow(s.hChild);
    }
    g_streams.clear();
    
    // Stop all timers
    KillTimer(g_hMainWnd, TIMER_PLAYER_CHECK);
    KillTimer(g_hMainWnd, TIMER_CHUNK_UPDATE);
    
    while (TabCtrl_GetItemCount(g_hTab) > 0)
        TabCtrl_DeleteItem(g_hTab, 0);
    ResizeTabAndChildren(g_hMainWnd);
}

// Settings dialog procedure
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG:
        // Initialize dialog controls with current settings
        SetDlgItemTextW(hDlg, IDC_PLAYERPATH, g_playerPath.c_str());
        SetDlgItemTextW(hDlg, IDC_PLAYERARGS, g_playerArg.c_str());
        CheckDlgButton(hDlg, IDC_MINIMIZETOTRAY, g_minimizeToTray ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_VERBOSE_DEBUG, g_verboseDebug ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LOG_TO_FILE, g_logToFile ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BROWSE_PLAYER: {
            OPENFILENAMEW ofn = { sizeof(ofn) };
            wchar_t szFile[MAX_PATH] = { 0 };
            
            GetDlgItemTextW(hDlg, IDC_PLAYERPATH, szFile, MAX_PATH);
            
            ofn.hwndOwner = hDlg;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"Executable Files\0*.exe\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrFileTitle = nullptr;
            ofn.nMaxFileTitle = 0;
            ofn.lpstrInitialDir = nullptr;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
            
            if (GetOpenFileNameW(&ofn)) {
                SetDlgItemTextW(hDlg, IDC_PLAYERPATH, szFile);
            }
            return TRUE;
        }
        case IDOK: {
            // Save settings
            wchar_t buffer[MAX_PATH];
            
            GetDlgItemTextW(hDlg, IDC_PLAYERPATH, buffer, MAX_PATH);
            g_playerPath = buffer;
            
            GetDlgItemTextW(hDlg, IDC_PLAYERARGS, buffer, MAX_PATH);
            g_playerArg = buffer;
            
            g_minimizeToTray = IsDlgButtonChecked(hDlg, IDC_MINIMIZETOTRAY) == BST_CHECKED;
            g_verboseDebug = IsDlgButtonChecked(hDlg, IDC_VERBOSE_DEBUG) == BST_CHECKED;
            g_logToFile = IsDlgButtonChecked(hDlg, IDC_LOG_TO_FILE) == BST_CHECKED;
            
            // Save settings to INI file
            SaveSettings();
            
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowSettingsDialog() {
    DialogBox(g_hInst, MAKEINTRESOURCE(IDD_SETTINGS), g_hMainWnd, SettingsDlgProc);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icex);
        RegisterStreamChildClass();
        HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_MYMENU));
        SetMenu(hwnd, hMenu);
        
        // Load accelerator table for hotkeys
        g_hAccel = LoadAccelerators(g_hInst, MAKEINTRESOURCE(IDR_MYACCEL));
        
        // Create Tahoma font to match original design (Font.Height = -11)
        g_hFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
        
        // Create favorites panel on the left
        HWND hFavLabel = CreateWindowEx(0, L"STATIC", L"Favorites:", WS_CHILD | WS_VISIBLE, 10, 10, 80, 18, hwnd, nullptr, g_hInst, nullptr);
        SendMessage(hFavLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        g_hFavoritesList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", 0, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 10, 30, 180, 300, hwnd, (HMENU)IDC_FAVORITES_LIST, g_hInst, nullptr);
        SendMessage(g_hFavoritesList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        // Favorites management buttons
        g_hFavoritesAdd = CreateWindowEx(0, L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 340, 40, 25, hwnd, (HMENU)IDC_FAVORITES_ADD, g_hInst, nullptr);
        SendMessage(g_hFavoritesAdd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        g_hFavoritesDelete = CreateWindowEx(0, L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 55, 340, 50, 25, hwnd, (HMENU)IDC_FAVORITES_DELETE, g_hInst, nullptr);
        SendMessage(g_hFavoritesDelete, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        g_hFavoritesEdit = CreateWindowEx(0, L"BUTTON", L"Edit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 110, 340, 40, 25, hwnd, (HMENU)IDC_FAVORITES_EDIT, g_hInst, nullptr);
        SendMessage(g_hFavoritesEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        g_hCheckVersion = CreateWindowEx(0, L"BUTTON", L"About", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 370, 100, 25, hwnd, (HMENU)IDC_CHECK_VERSION, g_hInst, nullptr);
        SendMessage(g_hCheckVersion, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        // Create stream tab control (main area)
        g_hTab = CreateWindowEx(0, WC_TABCONTROL, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE, 200, 10, 500, 300, hwnd, (HMENU)IDC_TAB, g_hInst, nullptr);
        SendMessage(g_hTab, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        // Create log list (at bottom)
        g_hLogList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 200, 320, 500, 120, hwnd, (HMENU)IDC_LOG_LIST, g_hInst, nullptr);
        SendMessage(g_hLogList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        InitLogList(g_hLogList);
        
        // Create status bar
        g_hStatusBar = CreateWindowEx(0, L"msctls_statusbar32", L"Buffer: 0 packets | GPAC Decoder Ready", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS_BAR, g_hInst, nullptr);
        SendMessage(g_hStatusBar, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        
        // Load favorites
        LoadFavorites();
        
        AddStreamTab();
        ResizeTabAndChildren(hwnd);
        UpdateAddFavoriteButtonState();
        break;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && g_minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            CreateTrayIcon();
        } else {
            ResizeTabAndChildren(hwnd);
        }
        break;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE && g_minimizeToTray) {
            ShowWindow(hwnd, SW_HIDE);
            CreateTrayIcon();
            return 0; // Prevent default minimize behavior
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    case WM_TRAYICON:
        if (wParam == ID_TRAYICON && lParam == WM_LBUTTONDBLCLK) {
            ShowFromTray();
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<LPNMHDR>(lParam)->hwndFrom == g_hTab &&
            reinterpret_cast<LPNMHDR>(lParam)->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(g_hTab);
            SwitchToTab(sel);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_NEWSTREAM:
            AddStreamTab(L"");
            break;
        case IDM_CLOSEACTIVE:
            CloseActiveTab();
            break;
        case IDM_CLOSEALL:
            CloseAllTabs();
            break;
        case IDM_EXIT:
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        case IDM_SETTINGS:
            ShowSettingsDialog();
            break;
        case IDC_FAVORITES_ADD:
            AddFavorite();
            break;
        case IDC_FAVORITES_DELETE:
            DeleteFavorite();
            break;
        case IDC_FAVORITES_EDIT:
            EditFavorite();
            break;
        case IDC_CHECK_VERSION:
            CheckVersion();
            break;
        case IDC_FAVORITES_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                OnFavoriteDoubleClick();
            } else if (HIWORD(wParam) == LBN_SELCHANGE) {
                OnFavoriteDoubleClick(); // Use same logic for single-click
            }
            break;
        }
        break;
    case WM_CLOSE:
        CloseAllTabs(); // Properly clean up all streaming tabs and media players
        DestroyWindow(hwnd);
        break;
    case WM_USER + 1: {
        // Thread-safe logging message
        std::wstring* msg = reinterpret_cast<std::wstring*>(lParam);
        if (msg) {
            AddLog(*msg);
            delete msg;
        }
        break;
    }
    case WM_USER + 2: {
        // Auto-stop stream when player exits
        size_t tabIndex = (size_t)wParam;
        AddDebugLog(L"WM_USER + 2: Auto-stop request for tab " + std::to_wstring(tabIndex) + 
                   L", streams.size=" + std::to_wstring(g_streams.size()));
        if (tabIndex < g_streams.size() && g_streams[tabIndex].isStreaming) {
            AddDebugLog(L"WM_USER + 2: Auto-stopping tab " + std::to_wstring(tabIndex) + 
                       L", channel=" + g_streams[tabIndex].channel);
            StopStream(g_streams[tabIndex]); // Auto-stop, not user-initiated
            AddLog(L"Stream stopped automatically (stream ended).");
        } else {
            AddDebugLog(L"WM_USER + 2: Invalid auto-stop request - tab " + std::to_wstring(tabIndex) + 
                       L" not streaming or out of range");
        }
        break;
    }
    case WM_TIMER:
        if (wParam == TIMER_PLAYER_CHECK) {
            // Update all streaming tabs that are still in "Starting..." state
            for (auto& tab : g_streams) {
                if (tab.isStreaming && !tab.playerStarted) {
                    SetWindowTextW(tab.hWatchBtn, L"Started");
                    tab.playerStarted = true;
                }
            }
            KillTimer(hwnd, TIMER_PLAYER_CHECK);
        } else if (wParam == TIMER_CHUNK_UPDATE) {
            // Check if any stream is active and get total chunk count
            // Also check for dead player processes
            bool hasActiveStream = false;
            int totalChunkCount = 0;
            
            for (auto& tab : g_streams) {
                if (tab.isStreaming) {
                    hasActiveStream = true;
                    totalChunkCount += tab.chunkCount.load();
                    
                    // Check if player process is still running
                    if (tab.playerProcess && tab.playerProcess != INVALID_HANDLE_VALUE) {
                        DWORD exitCode;
                        if (GetExitCodeProcess(tab.playerProcess, &exitCode)) {
                            if (exitCode != STILL_ACTIVE) {
                                // Player process has died - stop the stream
                                AddDebugLog(L"TIMER_CHUNK_UPDATE: Player process died for " + tab.channel + 
                                           L", exit code=" + std::to_wstring(exitCode));
                                AddLog(L"Media player closed for " + tab.channel + L" - stopping stream");
                                
                                // Simulate Stop button click to follow exact same code path as manual click
                                PostMessage(tab.hChild, WM_COMMAND, MAKEWPARAM(IDC_STOP, BN_CLICKED), (LPARAM)tab.hStopBtn);
                            }
                        } else {
                            // Failed to get exit code - might be dead process
                            DWORD error = GetLastError();
                            if (error == ERROR_INVALID_HANDLE) {
                                AddDebugLog(L"TIMER_CHUNK_UPDATE: Invalid player process handle for " + tab.channel + 
                                           L" - stopping stream");
                                AddLog(L"Media player connection lost for " + tab.channel + L" - stopping stream");
                                
                                // Simulate Stop button click to follow exact same code path as manual click
                                PostMessage(tab.hChild, WM_COMMAND, MAKEWPARAM(IDC_STOP, BN_CLICKED), (LPARAM)tab.hStopBtn);
                            }
                        }
                    }
                }
            }
            
            if (hasActiveStream) {
                // Show actual chunk queue count and frame statistics from streaming threads
                std::wstring status = L"Buffer: " + std::to_wstring(totalChunkCount) + L" packets";
                
                // Add frame information if available (for transport stream mode)
                // This would require accessing transport stream router stats, which we'll add as needed
                // For now, show basic buffer info
                
                UpdateStatusBar(status);
            } else {
                // No active streams, stop the timer
                KillTimer(hwnd, TIMER_CHUNK_UPDATE);
                UpdateStatusBar(L"Buffer: 0 packets | GPAC Decoder Ready");
            }
        }
        break;
    case WM_DESTROY:
        CloseAllTabs();
        RemoveTrayIcon();
        SaveSettings(); // Save settings including file logging preference
        if (g_hFont) {
            DeleteObject(g_hFont);
            g_hFont = nullptr;
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow) {
    g_hInst = hInstance;
    
    // Load settings from INI file
    LoadSettings();
    
    // Reserve sufficient capacity for streams vector to prevent reallocation
    // This prevents use-after-free bugs when running threads reference cancelToken
    g_streams.reserve(20);  // Support up to 20 concurrent streams without reallocation
    
    // Initialize TLS client system for fallback support
    TLSClientHTTP::Initialize();
    
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TardsplayaMainWin";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(0, L"TardsplayaMainWin", L"Tardsplaya",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);
    g_hMainWnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!g_hAccel || !TranslateAccelerator(g_hMainWnd, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
