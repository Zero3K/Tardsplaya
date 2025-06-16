#pragma once
#include <vector>
#include <string>

// Loads favorites from a text file (one channel name per line, UTF-8 or UTF-16)
std::vector<std::wstring> LoadFavoritesFromFile(const wchar_t* filename);

// Saves favorites to a text file (one channel name per line, UTF-8 or UTF-16)
bool SaveFavoritesToFile(const wchar_t* filename, const std::vector<std::wstring>& favorites);