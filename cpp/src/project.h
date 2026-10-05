// Проект страниц: данные, сохранение правок, экспорт (output.t42, output.html, html/).
#pragma once
#include "pages.h"

struct Project {
    std::string proj, outdir, base, edited_path, extras_path, quality_path;
    Json meta, extras;
    std::string stream, name, title, system, charset, charset2;
    int national = 1;

    bool open(const std::string &path);          // false — нет pages.json
    static bool is_project(const std::string &path);
    void set_charset(const std::string &cs);
    void set_charset2(const std::string &cs);
    const Charset &table(const Page *pg = nullptr) const;
    const Charset *table2() const;
    const Json *clock() const;
    std::string air_time(double t) const;
    std::string rel(const std::string &p) const;

    // -> страницы, изменённые, описание источника; бросает runtime_error
    Pages load_state(std::set<std::string> &edited, std::string &src) const;
    void save_state(const Pages &pages, const std::set<std::string> &edited) const;

    Over fitted(const Rows &rows, const std::map<std::string, std::string> *over, const Charset &t) const;
    const std::map<std::string, std::string> *overlay(const Page &pg, const Version *s) const;
    std::string row_text(const Row &b, const std::map<std::string, std::string> *over, int r, const Charset &t) const;
    std::string page_title(const Page &pg, const std::string &pid = "") const;     // раздел: середина заголовка (без номера, даты, часов)
    std::string header_middle(const Page &pg, const std::string &pid = "") const;
    std::string service_note(const Page &pg) const;
    std::string version_label(const Page &pg, size_t k) const;
    std::vector<Version> full_versions(const Page &pg) const;
    Pages full_pages(const Pages &pages) const;
    Bytes to_t42(const Pages &pages, int &n) const;
    std::string to_output_html(const Pages &pages) const;
    int to_page_htmls(const Pages &pages, const std::string &outdir) const;
    // clean — output.t42 без повторов: по одной собранной копии каждой подстраницы (как clean.t42)
    std::string export_all(const Pages &pages, bool full, bool clean = false) const;
    std::pair<std::string, int> export_srt(const std::string &page) const;
    // очищенный поток: по одной собранной (проголосованной) копии каждой подстраницы, по порядку номеров -> clean.t42
    std::string squash(const Pages &pages) const;
    // канал/служба, код сети, дата и время передачи — строка для показа рядом со страницей
    std::string service_line() const;
    // восстановление слов: редкое слово, которое отличается от частого слова этой же записи одним-двумя знаками
    // (и каждый знак — ошибкой в 1–2 битах), заменяется частым. changed — изменённые страницы (до правки — в before)
    struct WordFix { std::string page; std::string from, to; int n = 0; };
    std::vector<WordFix> restore_words(Pages &pages, std::map<std::string, Page> *before = nullptr) const;
};

bool over_fits(const std::string &ov, int c, const Charset &t);
