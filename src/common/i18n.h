#pragma once

#include <algorithm>
#include <format>
#include <nlohmann/json.hpp>
#include <string>

void LoadLangs();

nlohmann::json GetLangs();

bool LoadTranslation(const std::string& lang);

std::string tr(const char* key);

template <typename... Args>
inline std::string tr(const char* key, const Args&... args) {
    return std::vformat(tr(key), std::make_format_args(args...));
}

template <typename... Args>
inline char* trc(const char* key, const Args&... args) {
    std::string s   = tr(key, args...);
    char*       ret = new char[s.size() + 1];
    std::copy_n(s.c_str(), s.size() + 1, ret);
    return ret;
}
