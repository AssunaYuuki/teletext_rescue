#include "speller.h"
#include "ui/json.h"
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace {
#ifndef _WIN32
// Linux, macOS (the command-line tool): no web speller, the dictionary alone is used
std::string post(const wchar_t *, const wchar_t *, const std::string &) { return {}; }
#else
std::string post(const wchar_t *host, const wchar_t *path, const std::string &body)
{
    std::string out;
    HINTERNET s = WinHttpOpen(L"TeletextRescue", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return out;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    const wchar_t *hdr = L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (r && WinHttpSendRequest(r, hdr, (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD n = 0;
        while (WinHttpQueryDataAvailable(r, &n) && n) {
            std::string buf(n, 0); DWORD got = 0;
            if (!WinHttpReadData(r, buf.data(), n, &got)) break;
            out.append(buf.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}
#endif
std::string urlenc(const std::string &v)
{
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : v) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') o += (char)ch;
        else { o += '%'; o += hex[ch >> 4]; o += hex[ch & 15]; }
    }
    return o;
}
}

std::map<std::string, std::vector<std::string>> yandex_speller(const std::vector<std::string> &words, const std::string &lang)
{
    std::map<std::string, std::vector<std::string>> out;
    const size_t BATCH = 150;
    for (size_t b = 0; b < words.size(); b += BATCH) {
        size_t e = std::min(words.size(), b + BATCH);
        std::string body = "lang=" + urlenc(lang) + "&options=0";
        for (size_t i = b; i < e; i++) body += "&text=" + urlenc(words[i]);
        std::string rsp = post(L"speller.yandex.net", L"/services/spellservice.json/checkTexts", body);
        if (rsp.empty()) return out;                           // no network: the dictionary alone is used
        Json j;
        try { j = Json::parse(rsp); } catch (...) { return out; }
        if (!j.is_arr()) return out;
        for (size_t i = 0; i < j.size() && b + i < e; i++)
            for (auto &err : j[i].a) {
                if (err.get_str("word") != words[b + i]) continue;   // only whole-word findings
                auto *s = err.find("s");
                if (!s) continue;
                for (auto &v : s->a) if (v.is_str()) out[words[b + i]].push_back(v.s);
            }
    }
    return out;
}
