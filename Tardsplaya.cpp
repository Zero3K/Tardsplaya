#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <ctime>
#include <regex>
#include "resource.h"
#include "json_minimal.h"
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")

struct StreamTab {
    std::wstring channel;
    HWND hChild;
    HWND hQualities;
    HWND hWatchBtn;
    std::vector<std::wstring> qualities;
    std::map<std::wstring, std::wstring> qualityToUrl;
};

HINSTANCE g_hInst;
HWND g_hMainWnd, g_hTab, g_hLogList;
std::vector<StreamTab> g_streams;

std::wstring g_playerPath = L"mpv.exe";
std::wstring g_playerArg = L"-";
bool g_enableLogging = true;
bool g_logAutoScroll = true;

void AddLog(const std::wstring& msg) {
    if (!g_enableLogging) return;
    LVITEM item = { 0 };
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_hLogList);
    wchar_t timebuf[32];
    time_t now = time(0);
    struct tm tmval;
    localtime_s(&tmval, &now);
    wcsftime(timebuf, 32, L"%H:%M:%S", &tmval);
    item.pszText = timebuf;
    ListView_InsertItem(g_hLogList, &item);
    ListView_SetItemText(g_hLogList, item.iItem, 1, const_cast<LPWSTR>(msg.c_str()));
    if (g_logAutoScroll) ListView_EnsureVisible(g_hLogList, item.iItem, FALSE);
}

std::string WideToUtf8(const std::wstring& w) {
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(sz - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], sz, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(sz - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], sz);
    return out;
}

std::string HttpGet(const wchar_t* host, const wchar_t* path, const wchar_t* headers = nullptr) {
    HINTERNET hSession = WinHttpOpen(L"Tardsplaya/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return "";
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }
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
    return data;
}

std::wstring GetAccessToken(const std::wstring& channel) {
    std::wstring path = L"/api/channels/" + channel + L"/access_token";
    std::string resp = HttpGet(L"api.twitch.tv", path.c_str(), L"Client-ID: kimne78kx3ncx6brgo4mv6wki5h1ko");
    JsonValue jv = parse_json(resp);
    std::string token, sig;
    if (jv.type == JsonValue::Object) {
        token = jv["token"].as_str();
        sig = jv["sig"].as_str();
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
    std::wstring path = L"/api/channel/hls/" + channel +
        L".m3u8?player=twitchweb&allow_source=true&allow_audio_only=true&type=any&token=" +
        token + L"&sig=" + sig + L"&p=1";
    std::string resp = HttpGet(L"usher.ttvnw.net", path.c_str());
    return Utf8ToWide(resp);
}

std::map<std::wstring, std::wstring> ParsePlaylist(const std::wstring& m3u8) {
    std::map<std::wstring, std::wstring> result;
    std::wistringstream iss(m3u8);
    std::wstring line, quality, url;
    while (std::getline(iss, line)) {
        if (line.find(L"#EXT-X-STREAM-INF:") == 0) {
            std::wsmatch m;
            std::wregex rgx(L"VIDEO=\"([^\"]+)\"");
            if (std::regex_search(line, m, rgx))
                quality = m[1];
            else
                quality = L"unknown";
            std::getline(iss, url);
            result[quality] = url;
        }
    }
    if (result.empty() && m3u8.find(L"#EXTM3U") == 0) {
        std::wistringstream iss2(m3u8);
        while (std::getline(iss2, url)) {
            if (!url.empty() && url[0] != L'#')
                result[L"source"] = url;
        }
    }
    return result;
}

void RefreshQualities(StreamTab& tab) {
    SendMessage(tab.hQualities, LB_RESETCONTENT, 0, 0);
    for (const auto& q : tab.qualities)
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
    GetDlgItemText(tab.hChild, IDC_CHANNEL, channel, 128);
    if (wcslen(channel) == 0) {
        MessageBoxW(tab.hChild, L"Enter a channel name.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    tab.channel = channel;
    AddLog(L"Requesting Twitch access token...");
    std::wstring token = GetAccessToken(channel);
    if (token.empty()) {
        MessageBoxW(tab.hChild, L"Failed to get Twitch access token.", L"Error", MB_OK | MB_ICONERROR);
        AddLog(L"Failed to get Twitch access token.");
        return;
    }
    AddLog(L"Fetching playlist...");
    std::wstring m3u8 = FetchPlaylist(channel, token);
    if (m3u8.empty()) {
        MessageBoxW(tab.hChild, L"Failed to get playlist (stream may be offline).", L"Error", MB_OK | MB_ICONERROR);
        AddLog(L"Failed to get playlist.");
        return;
    }
    AddLog(L"Parsing qualities...");
    tab.qualityToUrl = ParsePlaylist(m3u8);
    tab.qualities.clear();
    for (const auto& pair : tab.qualityToUrl)
        tab.qualities.push_back(pair.first);
    RefreshQualities(tab);
    if (tab.qualities.empty()) {
        MessageBoxW(tab.hChild, L"No qualities found.", L"Error", MB_OK | MB_ICONERROR);
        EnableWindow(tab.hWatchBtn, FALSE);
    }
    else {
        EnableWindow(tab.hWatchBtn, TRUE);
    }
}

void WatchStream(StreamTab& tab) {
    int sel = (int)SendMessage(tab.hQualities, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        MessageBoxW(tab.hChild, L"Select a quality.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t qual[64];
    SendMessage(tab.hQualities, LB_GETTEXT, sel, (LPARAM)qual);
    auto it = tab.qualityToUrl.find(qual);
    if (it == tab.qualityToUrl.end()) {
        MessageBoxW(tab.hChild, L"Failed to resolve quality URL.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring url = it->second;
    std::wstring cmd = L"\"" + g_playerPath + L"\" " + g_playerArg + L" \"" + url + L"\"";
    AddLog(L"Launching: " + cmd);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxW(tab.hChild, L"Failed to launch media player.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

LRESULT CALLBACK StreamChildProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        // (Handled by parent)
        return 0;
    }
    if (msg == WM_COMMAND) {
        StreamTab* tab = reinterpret_cast<StreamTab*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!tab) return 0;
        switch (LOWORD(wParam)) {
        case IDC_LOAD:
            LoadChannel(*tab);
            break;
        case IDC_WATCH:
            WatchStream(*tab);
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
    RECT rc = { 0, 0, 480, 120 };
    HWND hwnd = CreateWindowEx(0, L"StreamChildWin", NULL,
        WS_CHILD | WS_VISIBLE,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hParent, nullptr, g_hInst, nullptr);
    CreateWindowEx(0, L"STATIC", L"Channel:", WS_CHILD | WS_VISIBLE, 10, 10, 55, 18, hwnd, nullptr, g_hInst, nullptr);
    HWND hEdit = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", channel, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70, 10, 140, 22, hwnd, (HMENU)IDC_CHANNEL, g_hInst, nullptr);
    HWND hLoad = CreateWindowEx(0, L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 220, 10, 60, 22, hwnd, (HMENU)IDC_LOAD, g_hInst, nullptr);
    CreateWindowEx(0, L"STATIC", L"Qualities:", WS_CHILD | WS_VISIBLE, 10, 40, 60, 18, hwnd, nullptr, g_hInst, nullptr);
    HWND hQualList = CreateWindowEx(WS_EX_CLIENTEDGE, L"LISTBOX", 0, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 70, 40, 140, 60, hwnd, (HMENU)IDC_QUALITIES, g_hInst, nullptr);
    HWND hWatch = CreateWindowEx(0, L"BUTTON", L"Watch", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 220, 40, 60, 22, hwnd, (HMENU)IDC_WATCH, g_hInst, nullptr);
    EnableWindow(hWatch, FALSE);

    tab.hChild = hwnd;
    tab.hQualities = hQualList;
    tab.hWatchBtn = hWatch;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)&tab);
    return hwnd;
}

void ResizeTabAndChildren(HWND hwnd) {
    RECT rcMain;
    GetClientRect(hwnd, &rcMain);
    int logHeight = 120;
    SetWindowPos(g_hTab, nullptr, 0, 0, rcMain.right, rcMain.bottom - logHeight, SWP_NOZORDER);
    SetWindowPos(g_hLogList, nullptr, 0, rcMain.bottom - logHeight, rcMain.right, logHeight, SWP_NOZORDER);
    int sel = TabCtrl_GetCurSel(g_hTab);
    for (size_t i = 0; i < g_streams.size(); ++i) {
        ShowWindow(g_streams[i].hChild, i == sel ? SW_SHOW : SW_HIDE);
        if (i == sel) {
            RECT rcTab;
            GetClientRect(g_hTab, &rcTab);
            TabCtrl_AdjustRect(g_hTab, FALSE, &rcTab);
            SetWindowPos(g_streams[i].hChild, nullptr, rcTab.left, rcTab.top, rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }
}

void AddStreamTab(const std::wstring& channel = L"") {
    TCITEM tie = { 0 };
    tie.mask = TCIF_TEXT;
    tie.pszText = (LPWSTR)(channel.empty() ? L"New Stream" : channel.c_str());
    int idx = TabCtrl_GetItemCount(g_hTab);
    TabCtrl_InsertItem(g_hTab, idx, &tie);
    StreamTab tab = {};
    HWND hChild = CreateStreamChild(g_hTab, tab, channel.c_str());
    tab.hChild = hChild;
    g_streams.push_back(tab);
    TabCtrl_SetCurSel(g_hTab, idx);
    ResizeTabAndChildren(g_hMainWnd);
}

void SwitchToTab(int idx) {
    if (idx < 0 || idx >= (int)g_streams.size()) return;
    TabCtrl_SetCurSel(g_hTab, idx);
    ResizeTabAndChildren(g_hMainWnd);
    SetFocus(g_streams[idx].hChild);
}

void CloseActiveTab() {
    int cur = TabCtrl_GetCurSel(g_hTab);
    if (cur < 0 || cur >= (int)g_streams.size()) return;
    DestroyWindow(g_streams[cur].hChild);
    g_streams.erase(g_streams.begin() + cur);
    TabCtrl_DeleteItem(g_hTab, cur);
    if (!g_streams.empty()) {
        int newIdx = cur < (int)g_streams.size() ? cur : (int)g_streams.size() - 1;
        SwitchToTab(newIdx);
    }
    ResizeTabAndChildren(g_hMainWnd);
}

void CloseAllTabs() {
    for (auto& s : g_streams) DestroyWindow(s.hChild);
    g_streams.clear();
    while (TabCtrl_GetItemCount(g_hTab) > 0)
        TabCtrl_DeleteItem(g_hTab, 0);
    ResizeTabAndChildren(g_hMainWnd);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icex);
        RegisterStreamChildClass();
        HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDR_MYMENU));
        SetMenu(hwnd, hMenu);
        g_hTab = CreateWindowEx(0, WC_TABCONTROL, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE, 0, 0, 100, 100, hwnd, (HMENU)IDC_TAB, g_hInst, nullptr);
        g_hLogList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 0, 0, 100, 100, hwnd, (HMENU)IDC_LOG_LIST, g_hInst, nullptr);
        InitLogList(g_hLogList);
        AddStreamTab();
        ResizeTabAndChildren(hwnd);
        break;
    }
    case WM_SIZE:
        ResizeTabAndChildren(hwnd);
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
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        CloseAllTabs();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInstance;
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TardsplayaMainWin";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClass(&wc);
    HWND hwnd = CreateWindowEx(0, L"TardsplayaMainWin", L"Tardsplaya",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, hInstance, nullptr);
    g_hMainWnd = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}