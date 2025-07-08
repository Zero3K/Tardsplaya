#include "tlsclient.h"
#include <sstream>
#include <algorithm>
#include <winhttp.h>
#include "../chunked_decode.h"

#define NOMINMAX

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

static bool g_tlsInitialized = false;

// Forward declaration - WideToUtf8 is defined in Tardsplaya.cpp
extern std::string WideToUtf8(const std::wstring& wide);

// Helper: Get HTTP body (skip headers) - from TLS client example
std::string get_http_body(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return resp.substr(pos + 4);
}

TLSClient::TLSClient() {
    lastError = "";
}

TLSClient::~TLSClient() {
    // Cleanup if needed
}

void TLSClient::InitializeGlobal() {
    if (!g_tlsInitialized) {
        // Initialize Winsock
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        g_tlsInitialized = true;
        
        // Note: TLS client initialization will be called from tlsclient_source.cpp when needed
    }
}

bool TLSClient::ParseUrl(const std::string& url, std::string& host, int& port, std::string& path, bool& isHttps) {
    // Simple URL parsing for HTTP/HTTPS
    if (url.substr(0, 8) == "https://") {
        isHttps = true;
        port = 443;
        std::string remainder = url.substr(8);
        size_t slashPos = remainder.find('/');
        if (slashPos != std::string::npos) {
            host = remainder.substr(0, slashPos);
            path = remainder.substr(slashPos);
        } else {
            host = remainder;
            path = "/";
        }
    } else if (url.substr(0, 7) == "http://") {
        isHttps = false;
        port = 80;
        std::string remainder = url.substr(7);
        size_t slashPos = remainder.find('/');
        if (slashPos != std::string::npos) {
            host = remainder.substr(0, slashPos);
            path = remainder.substr(slashPos);
        } else {
            host = remainder;
            path = "/";
        }
    } else {
        lastError = "Invalid URL scheme";
        return false;
    }

    // Check for port in host
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = std::stoi(host.substr(colonPos + 1));
        host = host.substr(0, colonPos);
    }

    return true;
}

bool TLSClient::ParseUrlW(const std::wstring& url, std::string& host, int& port, std::string& path, bool& isHttps) {
    // Convert wide string to string for parsing
    std::string urlStr = WideToUtf8(url);
    return ParseUrl(urlStr, host, port, path, isHttps);
}

bool TLSClient::HttpGet(const std::string& url, std::string& response, const std::string& headers) {
    std::string host, path;
    int port;
    bool isHttps;
    
    if (!ParseUrl(url, host, port, path, isHttps)) {
        lastError = "Failed to parse URL";
        return false;
    }
    
    // Convert to wide strings for WinHTTP
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wHeaders(headers.begin(), headers.end());
    
    // Use WinHTTP with improved error handling
    HINTERNET hSession = WinHttpOpen(L"Tardsplaya TLS Client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        lastError = "Failed to open WinHTTP session";
        return false;
    }
    
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        lastError = "Failed to connect to host";
        return false;
    }
    
    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        lastError = "Failed to create request";
        return false;
    }
    
    // For HTTPS, ignore certificate errors for better compatibility
    if (isHttps) {
        DWORD dwSecurityFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                               SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                               SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                               SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags));
    }
    
    const wchar_t* requestHeaders = wHeaders.empty() ? NULL : wHeaders.c_str();
    BOOL bResult = WinHttpSendRequest(hRequest, requestHeaders, requestHeaders ? -1 : 0, 
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL);
    
    if (bResult) {
        DWORD dwSize = 0;
        do {
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            
            size_t prevSize = response.size();
            response.resize(prevSize + dwSize);
            WinHttpReadData(hRequest, &response[prevSize], dwSize, &dwDownloaded);
            if (dwDownloaded < dwSize) {
                response.resize(prevSize + dwDownloaded);
            }
        } while (dwSize > 0);
    } else {
        lastError = "Failed to send request or receive response";
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return bResult != FALSE;
}

bool TLSClient::HttpGetW(const std::wstring& url, std::string& response, const std::wstring& headers) {
    std::string urlStr = WideToUtf8(url);
    std::string headersStr = WideToUtf8(headers);
    return HttpGet(urlStr, response, headersStr);
}

bool TLSClient::HttpPost(const std::string& url, const std::string& postData, std::string& response, const std::string& headers) {
    std::string host, path;
    int port;
    bool isHttps;
    
    if (!ParseUrl(url, host, port, path, isHttps)) {
        lastError = "Failed to parse URL";
        return false;
    }
    
    // Convert to wide strings for WinHTTP
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wHeaders(headers.begin(), headers.end());
    
    // Use WinHTTP with improved error handling
    HINTERNET hSession = WinHttpOpen(L"Tardsplaya TLS Client/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        lastError = "Failed to open WinHTTP session";
        return false;
    }
    
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        lastError = "Failed to connect to host";
        return false;
    }
    
    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        lastError = "Failed to create request";
        return false;
    }
    
    // For HTTPS, ignore certificate errors for better compatibility
    if (isHttps) {
        DWORD dwSecurityFlags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                               SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                               SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                               SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwSecurityFlags, sizeof(dwSecurityFlags));
    }
    
    const wchar_t* requestHeaders = wHeaders.empty() ? NULL : wHeaders.c_str();
    BOOL bResult = WinHttpSendRequest(hRequest, requestHeaders, requestHeaders ? -1 : 0, 
        (LPVOID)postData.c_str(), postData.length(), postData.length(), 0) && WinHttpReceiveResponse(hRequest, NULL);
    
    if (bResult) {
        DWORD dwSize = 0;
        do {
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;
            
            size_t prevSize = response.size();
            response.resize(prevSize + dwSize);
            WinHttpReadData(hRequest, &response[prevSize], dwSize, &dwDownloaded);
            if (dwDownloaded < dwSize) {
                response.resize(prevSize + dwDownloaded);
            }
        } while (dwSize > 0);
    } else {
        lastError = "Failed to send request or receive response";
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return bResult != FALSE;
}

bool TLSClient::HttpPostW(const std::wstring& url, const std::string& postData, std::string& response, const std::wstring& headers) {
    std::string urlStr = WideToUtf8(url);
    std::string headersStr = WideToUtf8(headers);
    return HttpPost(urlStr, postData, response, headersStr);
}

// Global wrapper functions - these will be the main integration points
namespace TLSClientHTTP {
    void Initialize() {
        TLSClient::InitializeGlobal();
    }

    std::string HttpGet(const std::wstring& host, const std::wstring& path, const std::wstring& headers) {
        std::wstring url = L"https://" + host + path;
        TLSClient client;
        std::string response;
        
        if (client.HttpGetW(url, response, headers)) {
            // Extract body from HTTP response using helper function
            return get_http_body(response);
        }
        
        return "";
    }

    std::string HttpPost(const std::wstring& host, const std::wstring& path, const std::string& postData, const std::wstring& headers) {
        std::wstring url = L"https://" + host + path;
        TLSClient client;
        std::string response;
        
        if (client.HttpPostW(url, postData, response, headers)) {
            // Extract body from HTTP response using helper function
            return get_http_body(response);
        }
        
        return "";
    }

    bool HttpGetText(const std::wstring& url, std::string& out) {
        TLSClient client;
        std::string response;
        
        if (client.HttpGetW(url, response)) {
            // Extract body from HTTP response using helper function
            out = get_http_body(response);
            return !out.empty();
        }
        
        return false;
    }
}