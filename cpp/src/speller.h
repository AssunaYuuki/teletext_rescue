// Yandex Speller (speller.yandex.net, free, no key): spelling suggestions for words.
#pragma once
#include <map>
#include <string>
#include <vector>

// words and suggestions in UTF-8; lang "ru,en" or "en" (the service knows ru, uk, en).
// Words without suggestions are left out; an empty map also means "no network".
std::map<std::string, std::vector<std::string>> yandex_speller(const std::vector<std::string> &words, const std::string &lang);
