// Небольшой JSON: значение, разбор, вывод (UTF-8 как есть, как ensure_ascii=False).
#pragma once
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    Json() {}
    Json(std::nullptr_t) {}
    Json(bool v) : t(Bool), b(v) {}
    Json(int v) : t(Num), n(v) {}
    Json(long v) : t(Num), n((double)v) {}
    Json(long long v) : t(Num), n((double)v) {}
    Json(unsigned v) : t(Num), n(v) {}
    Json(size_t v) : t(Num), n((double)v) {}
    Json(double v) : t(Num), n(v) {}
    Json(const char *v) : t(Str), s(v) {}
    Json(const std::string &v) : t(Str), s(v) {}
    static Json array() { Json j; j.t = Arr; return j; }
    static Json object() { Json j; j.t = Obj; return j; }

    bool is_null() const { return t == Null; }
    bool is_obj() const { return t == Obj; }
    bool is_arr() const { return t == Arr; }
    bool is_str() const { return t == Str; }
    bool is_num() const { return t == Num; }
    bool truthy() const;

    // объект
    const Json *find(const std::string &k) const;
    Json *find(const std::string &k);
    bool has(const std::string &k) const { return find(k) != nullptr; }
    const Json &operator[](const std::string &k) const;
    Json &operator[](const std::string &k);              // создаёт ключ
    void erase(const std::string &k);
    // массив
    const Json &operator[](size_t i) const { return a[i]; }
    Json &operator[](size_t i) { return a[i]; }
    Json &push(const Json &v) { if (t != Arr) { t = Arr; } a.push_back(v); return a.back(); }
    size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }

    // значения с умолчанием
    std::string str(const std::string &d = "") const { return t == Str ? s : d; }
    double num(double d = 0) const { return t == Num ? n : t == Bool ? (b ? 1 : 0) : d; }
    int integer(int d = 0) const { return t == Num ? (int)n : t == Bool ? (int)b : d; }
    std::string get_str(const std::string &k, const std::string &d = "") const { auto p = find(k); return p ? p->str(d) : d; }
    double get_num(const std::string &k, double d = 0) const { auto p = find(k); return p && p->t == Num ? p->n : d; }
    bool get_bool(const std::string &k, bool d = false) const { auto p = find(k); return p ? p->truthy() : d; }

    std::string dump(int indent = -1) const;           // -1 — компактно
    static Json parse(const std::string &text);        // бросает std::runtime_error
};

Json load_json(const std::string &path, const Json &dflt = Json());
void save_json(const std::string &path, const Json &j, int indent = -1);
