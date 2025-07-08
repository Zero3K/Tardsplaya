#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

// Include winsock2.h before windows.h to avoid macro redefinition warnings
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>

// TLS Client wrapper for HTTP operations
// Provides an alternative to WinHTTP for better Windows 7 compatibility
class TLSClient {
public:
    TLSClient();
    ~TLSClient();

    // Initialize global TLS subsystem (call once)
    static void InitializeGlobal();

    // Perform HTTP GET request using TLS client
    // Returns true on success, false on failure
    bool HttpGet(const std::string& url, std::string& response, const std::string& headers = "");
    
    // Perform HTTP GET request (wide string version)
    bool HttpGetW(const std::wstring& url, std::string& response, const std::wstring& headers = L"");

    // Get the last error message
    std::string GetLastError() const { return lastError; }

private:
    std::string lastError;
    bool ParseUrl(const std::string& url, std::string& host, int& port, std::string& path, bool& isHttps);
    bool ParseUrlW(const std::wstring& url, std::string& host, int& port, std::string& path, bool& isHttps);
};

// Global functions for easy integration with existing code
// These can be used as drop-in replacements for WinHTTP functions
namespace TLSClientHTTP {
    // Initialize TLS client system
    void Initialize();
    
    // HTTP GET with TLS client (returns response as string)
    std::string HttpGet(const std::wstring& host, const std::wstring& path, const std::wstring& headers = L"");
    
    // HTTP GET with full URL
    bool HttpGetText(const std::wstring& url, std::string& out);
}