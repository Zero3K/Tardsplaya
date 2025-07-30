#pragma once
//
// Adapted Simple HLS Client - HLS Fetcher for Tardsplaya (Windows WinHTTP version)
// Original from https://github.com/bytems/simple_hls_client
//

#ifndef SIMPLE_HLS_CLIENT_HLS_FETCHER_H
#define SIMPLE_HLS_CLIENT_HLS_FETCHER_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <winhttp.h>
#include <winerror.h>
#include <string>
#include <stdexcept>

// Forward declare TLS client for fallback
namespace tlsclient {
    bool get_https_content(const std::string& url, std::string& response);
}

/**
 * @brief HLS content fetcher using WinHTTP (with TLS client fallback).
 * 
 * This class replaces the libcurl dependency with Windows-native WinHTTP
 * and falls back to the custom TLS client when needed.
 */
class HLSFetcher {
private:
    std::string url_;
    std::string response_data_;
    DWORD last_error_;

    // Convert std::string to std::wstring
    std::wstring StringToWString(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }

    // Convert std::wstring to std::string
    std::string WStringToString(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    // Parse URL components
    bool parseUrl(const std::string& url, std::wstring& hostname, std::wstring& path, INTERNET_PORT& port, bool& useHttps) {
        std::wstring wurl = StringToWString(url);
        
        // Check for HTTPS
        useHttps = (wurl.find(L"https://") == 0);
        size_t schemeEnd = useHttps ? 8 : 7; // "https://" or "http://"
        
        if (!useHttps && wurl.find(L"http://") != 0) {
            return false; // Invalid scheme
        }

        // Find hostname end
        size_t pathStart = wurl.find(L'/', schemeEnd);
        if (pathStart == std::wstring::npos) {
            pathStart = wurl.length();
            path = L"/";
        } else {
            path = wurl.substr(pathStart);
        }

        std::wstring hostPort = wurl.substr(schemeEnd, pathStart - schemeEnd);
        
        // Check for port
        size_t colonPos = hostPort.find(L':');
        if (colonPos != std::wstring::npos) {
            hostname = hostPort.substr(0, colonPos);
            port = (INTERNET_PORT)std::stoi(hostPort.substr(colonPos + 1));
        } else {
            hostname = hostPort;
            port = useHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        }

        return true;
    }

public:
    explicit HLSFetcher(const std::string& url) : url_(url), last_error_(0) {}

    /**
     * @brief Fetch content from the specified URL.
     * @return true if successful, false otherwise.
     */
    bool fetch() {
        response_data_.clear();
        last_error_ = 0;

        std::wstring hostname, path;
        INTERNET_PORT port;
        bool useHttps;

        if (!parseUrl(url_, hostname, path, port, useHttps)) {
            last_error_ = ERROR_INVALID_PARAMETER;
            return false;
        }

        // Try WinHTTP first
        if (fetchWithWinHTTP(hostname, path, port, useHttps)) {
            return true;
        }

        // Fallback to custom TLS client for HTTPS
        if (useHttps) {
            return fetchWithTLSClient();
        }

        return false;
    }

    /**
     * @brief Get the fetched response data.
     * @return Reference to the response string.
     */
    const std::string& getResponse() const {
        return response_data_;
    }

    /**
     * @brief Get the last error code.
     * @return Windows error code.
     */
    DWORD getLastError() const {
        return last_error_;
    }

private:
    bool fetchWithWinHTTP(const std::wstring& hostname, const std::wstring& path, INTERNET_PORT port, bool useHttps) {
        HINTERNET hSession = WinHttpOpen(
            L"Tardsplaya-HLS-Client/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!hSession) {
            last_error_ = GetLastError();
            return false;
        }

        // Set timeouts
        WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 30000);

        HINTERNET hConnect = WinHttpConnect(hSession, hostname.c_str(), port, 0);
        if (!hConnect) {
            last_error_ = GetLastError();
            WinHttpCloseHandle(hSession);
            return false;
        }

        DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            L"GET",
            path.c_str(),
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags
        );

        if (!hRequest) {
            last_error_ = GetLastError();
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // For HTTPS, disable certificate validation (for compatibility)
        if (useHttps) {
            DWORD security_flags = SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                   SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                   SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                   SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &security_flags, sizeof(security_flags));
        }

        BOOL result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!result) {
            last_error_ = GetLastError();
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        result = WinHttpReceiveResponse(hRequest, NULL);
        if (!result) {
            last_error_ = GetLastError();
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Check status code
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
                           NULL, &statusCode, &statusCodeSize, NULL);

        if (statusCode != 200) {
            last_error_ = statusCode;
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Read response data
        DWORD bytesAvailable = 0;
        do {
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                last_error_ = GetLastError();
                break;
            }

            if (bytesAvailable > 0) {
                std::vector<char> buffer(bytesAvailable + 1);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                    buffer[bytesRead] = '\0';
                    response_data_.append(buffer.data(), bytesRead);
                } else {
                    last_error_ = GetLastError();
                    break;
                }
            }
        } while (bytesAvailable > 0);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        return last_error_ == 0;
    }

    bool fetchWithTLSClient() {
        try {
            return tlsclient::get_https_content(url_, response_data_);
        } catch (...) {
            last_error_ = ERROR_INTERNET_CONNECTION_ABORTED;
            return false;
        }
    }
};

#endif // SIMPLE_HLS_CLIENT_HLS_FETCHER_H