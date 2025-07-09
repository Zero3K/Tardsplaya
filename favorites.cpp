#include "favorites.h"
#include <fstream>
#include <sstream>
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// Helper function to convert UTF-8 to wide string (local version)
static std::wstring Utf8ToWideLocal(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], size);
    return result;
}

// Helper function to convert wide string to UTF-8 (local version)
static std::string WideToUtf8Local(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

std::vector<std::wstring> LoadFavoritesFromFile(const wchar_t* filename) {
    std::vector<std::wstring> favs;

    // Try to open as binary first to detect BOM
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) return favs;

    // Read the entire file
    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    if (size == 0) return favs;
    
    std::vector<char> buffer(size);
    file.read(buffer.data(), size);
    file.close();

    // Check for UTF-16 BOM
    if (size >= 2 && (unsigned char)buffer[0] == 0xFF && (unsigned char)buffer[1] == 0xFE) {
        // UTF-16 LE BOM detected
        const wchar_t* wdata = reinterpret_cast<const wchar_t*>(buffer.data() + 2);
        size_t wsize = (size - 2) / sizeof(wchar_t);
        std::wstring content(wdata, wsize);
        
        std::wistringstream ss(content);
        std::wstring line;
        while (std::getline(ss, line)) {
            // Remove carriage return if present
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                favs.push_back(line);
            }
        }
    } else {
        // Assume UTF-8 (or ASCII)
        std::string content(buffer.begin(), buffer.end());
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty()) {
                favs.push_back(Utf8ToWideLocal(line));
            }
        }
    }
    
    return favs;
}

bool SaveFavoritesToFile(const wchar_t* filename, const std::vector<std::wstring>& favorites) {
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) return false;

    // Write UTF-8 BOM
    const char bom[] = { (char)0xEF, (char)0xBB, (char)0xBF };
    file.write(bom, 3);

    for (const auto& fav : favorites) {
        std::string utf8_line = WideToUtf8Local(fav);
        file.write(utf8_line.c_str(), utf8_line.length());
        file.write("\n", 1);
    }
    
    return true;
}
