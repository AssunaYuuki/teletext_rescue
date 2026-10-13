// Страницы телетекста: версии, ряды, служебные данные; чтение и запись pages.json.
#pragma once
#include "json.h"
#include "teletext.h"

struct Version {
    double t = 0;              // секунды записи
    int n = 0;                 // передач
    Rows rows;                 // ряд -> 40 кодов
    std::map<int, int> c;      // ряд -> сколько копий подтвердили
    std::string s;             // код подстраницы (S4 S3 S2 S1), пусто — нет
    bool full = false;         // полная страница (собрана из всех версий)
};

using PagesBuild = std::map<std::string, std::vector<Version>>;   // pages.json

struct PageExtras {
    std::vector<std::string> flof;            // 5 ссылок ("" — нет); пусто — ключей нет
    std::map<std::string, std::string> x26;   // "r,c" -> символ
    std::map<std::string, std::map<std::string, std::string>> x26s;   // подстраница -> поправки
    bool boxed = false;
    int tx = 0, subpages = 1, nat = -1;       // nat -1 — не известен
};

struct Page {
    std::vector<Version> versions;
    bool deleted = false;
    PageExtras ex;
};
using Pages = std::map<std::string, Page>;

Json version_to_json(const Version &v);
Version version_from_json(const Json &j);
Json build_to_json(const PagesBuild &p);
PagesBuild build_from_json(const Json &j);
Json extras_to_json(const PageExtras &e);
PageExtras extras_from_json(const Json &j);
Json page_to_json(const Page &p);
Page page_from_json(const Json &j);
Row row_from_json(const Json &a);
Json row_to_json(const Row &r);
