// NABTS (.t33) — разбор и показ страниц NAPLPS (CBS ExtraVision, NBC Teletext).
// Пакет 33 байта (CEA-516 §3), группы данных (§4), записи и серии (§5), каталог с голосованием
// копий, цепочки More, интерпретатор NAPLPS (ANSI X3.110) и растр приёмника.
#pragma once
#include "json.h"
#include "png.h"

namespace nabts {
constexpr double DISPLAY_H = 0.78125;

struct Col { int g = 0, r = 0, b = 0; bool t = false;
    bool operator==(const Col &o) const { return g == o.g && r == o.r && b == o.b && t == o.t; }
    bool operator!=(const Col &o) const { return !(*this == o); }
    bool operator<(const Col &o) const { return std::tie(g, r, b, t) < std::tie(o.g, o.r, o.b, o.t); } };
using Pt = std::pair<double, double>;

struct Blink { bool set = false; int to = 0; bool has_col = false; Col col; int on = 0, off = 0, delay = 0; double start = 0;
    bool operator==(const Blink &o) const { return set == o.set && (!set || (to == o.to && has_col == o.has_col && col == o.col && on == o.on && off == o.off && delay == o.delay && start == o.start)); } };

struct Prim {
    enum Kind { CHAR, POINT, LINE, ARC, RECT, POLY, INCR } kind = POINT;
    std::vector<Pt> points; Pt origin{0, 0}, size{0, 0};
    bool filled = false, highlighted = false; Pt pel{0, 0}; int line_tex = 0, pattern = 0; Pt mask_size{0, 0};
    int mode = 0; Col colour{7, 7, 7, false}, background; int caddr = -1, baddr = -1;
    bool blinking = false; Col blink_to; int blink_addr = -1;
    std::vector<int> incr; int ch = 0; char rep = 'P'; int rotation = 0, path = 0; bool reverse = false, underlined = false;
    double t = 0; int daddr = 0;
};
struct DrcsChar { int code = 0, w = 0, h = 0; std::vector<char> el; };
struct Mask { int w = 0, h = 0; std::vector<char> el; bool defined() const { return w > 0 && h > 0; } };
struct Event { char type; double t; std::shared_ptr<Prim> p; std::array<Col, 16> map; std::array<Blink, 16> blink; };   // 'p','c','m','b'

struct Page {
    std::vector<std::shared_ptr<Prim>> prims;
    std::array<Col, 16> colour_map;
    std::map<int, DrcsChar> drcs;
    std::array<Mask, 4> masks;
    std::vector<Event> events;
    double end = 0;
};

struct Record {
    int channel = 0; long long address = 0; int version = 0; bool long_form = false;
    int type = 0, records = 0; bool complete = false;
    Bytes data, present; bool has_more = false; long long more_address = 0;
    int seen = 0, intact_n = 0, attested_n = 0, copies_voted = 0; long first = 0, last = 0;
    std::map<std::string, bool> flags;
    std::string addr_text, purpose;
    std::shared_ptr<Page> page; std::string text; long long chain_base = 0; int chain_pos = 0;
};
struct Summary { long packets = 0; std::map<std::string, long> groups; std::map<std::pair<int, int>, long> foreign; int folded = 0, dropped = 0; };

std::vector<Record> read_t33(const std::string &path, Summary &summ, Progress *pr = nullptr);
void interpret(std::vector<Record> &records, int gw = 256, int gh = 200);
std::string record_label(const Record &r);
std::string flags_text(const Record &r);
std::string address_text(long long a);
std::string record_name(const Record &r);      // %03X-адрес-vN

class Player {
public:
    Player(const Page &page, int gw = 256, int gh = 200);
    bool advance(double T, bool all = false);    // all — все события
    bool done() const { return i_ >= ev().size(); }
    bool has_blink() const { return has_blink_; }
    std::vector<Col> blink_key(double T) const;
    Image image(double T, bool blink_on_phase = false) const;   // T < 0 — мигание «включено»
    double end() const { return page_.end; }
    // для HTML: журнал записанных клеток с прошлого снимка
    std::map<int, int> log; bool logging = false;
    int surf_id = 0, map_ver = 0, blink_ver = 0;     // меняются при очистке экрана / смене карты / мигания
    std::array<Col, 16> map; std::array<Blink, 16> blink;
    struct Ink { char k; Col c; int a; bool operator<(const Ink &o) const { return std::tie(k, c, a) < std::tie(o.k, o.c, o.a); } };
    std::vector<Ink> inks;
    int gw, gh;
    int pen(const Ink &s);
    std::vector<int> cells;                           // -1 — пусто, иначе номер чернил
    Col colour(const Ink &s, double T) const;
private:
    const std::vector<Event> &ev() const { return page_.events; }
    const Page &page_;
    size_t i_ = 0; bool has_blink_ = false;
    std::map<Ink, int> ink_id;
};
Image render_page(const Page &page, int gw = 256, int gh = 200);
uint32_t rgb(const Col &c);
}

// trcli / программа
void nabts_dump(const std::string &t33, const std::string &out, Progress &pr);
int nabts_html_export(const std::vector<nabts::Record> &recs, const std::string &out, int gw, int gh, Progress &pr);
void nabts_html_export_file(const std::string &t33, const std::string &out, Progress &pr);
