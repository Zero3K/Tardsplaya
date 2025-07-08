#include "tlsclient.h"
#include <sstream>
#include <algorithm>
#include <winhttp.h>

#define NOMINMAX

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

static bool g_tlsInitialized = false;

// Helper function to convert wide string to UTF-8
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wide[0], (int)wide.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wide[0], (int)wide.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
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
    // For now, use WinHTTP implementation with better error handling
    // TODO: Replace with actual TLS client implementation
    
    std::string host, path;
    int port;
    bool isHttps;
    
    if (!ParseUrl(url, host, port, path, isHttps)) {
        return false;
    }
    
    // Convert to wide strings for WinHTTP
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());
    std::wstring wHeaders(headers.begin(), headers.end());
    
    // Use WinHTTP for now
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
            // Extract body from HTTP response
            size_t headerEnd = response.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                return response.substr(headerEnd + 4);
            }
            // If no standard header separator found, try just LF
            headerEnd = response.find("\n\n");
            if (headerEnd != std::string::npos) {
                return response.substr(headerEnd + 2);
            }
            // Return full response if no headers found
            return response;
        }
        
        return "";
    }

    bool HttpGetText(const std::wstring& url, std::string& out) {
        TLSClient client;
        std::string response;
        
        if (client.HttpGetW(url, response)) {
            // Extract body from HTTP response
            size_t headerEnd = response.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                out = response.substr(headerEnd + 4);
                return true;
            }
            // If no standard header separator found, try just LF
            headerEnd = response.find("\n\n");
            if (headerEnd != std::string::npos) {
                out = response.substr(headerEnd + 2);
                return true;
            }
            // Return full response if no headers found
            out = response;
            return true;
        }
        
        return false;
    }
}