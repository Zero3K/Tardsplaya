#include "favorites.h"
#include <fstream>
#include <locale>
#include <codecvt>

std::vector<std::wstring> LoadFavoritesFromFile(const wchar_t* filename) {
    std::vector<std::wstring> favs;

    std::wifstream file(filename);
    if (!file.is_open()) return favs;

    file.imbue(std::locale(file.getloc(), new std::codecvt_utf8<wchar_t>));
    std::wstring line;
    while (std::getline(file, line)) {
        if (!line.empty())
            favs.push_back(line);
    }
    return favs;
}

bool SaveFavoritesToFile(const wchar_t* filename, const std::vector<std::wstring>& favorites) {
    std::wofstream file(filename);
    if (!file.is_open()) return false;

    file.imbue(std::locale(file.getloc(), new std::codecvt_utf8<wchar_t>));
    for (const auto& fav : favorites)
        file << fav << L"\n";
    return true;
}