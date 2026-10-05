// Страницы NABTS/NAPLPS в HTML с показом «как на экране приёмника».
#include "nabts.h"

using namespace nabts;

static Json rgbj(const Col &c) { uint32_t v = rgb(c); Json a = Json::array(); a.push((int)(v >> 16)); a.push((int)((v >> 8) & 255)); a.push((int)(v & 255)); return a; }
static Json mapj(const std::array<Col, 16> &m) { Json a = Json::array(); for (auto &c : m) a.push(rgbj(c)); return a; }
static Json blinkj(const std::array<Blink, 16> &b) {
    Json a = Json::array();
    for (auto &e : b) {
        if (!e.set) { a.push(Json()); continue; }
        Json x = Json::array(); x.push(e.to); x.push(e.to < 0 ? rgbj(e.col) : Json()); x.push(e.on); x.push(e.off); x.push(e.delay); x.push(round(e.start * 1000) / 1000);
        a.push(x);
    }
    return a;
}
static Json runs(const std::map<int, int> &log) {
    Json out = Json::array(); std::vector<long> v;
    for (auto &kv : log) {
        size_t n = v.size();
        if (n && v[n - 3] + v[n - 2] == kv.first && v[n - 1] == kv.second) v[n - 2]++;
        else { v.push_back(kv.first); v.push_back(1); v.push_back(kv.second); }
    }
    for (long x : v) out.push(x);
    return out;
}

static Json page_data(const Page &page, int gw, int gh) {
    Player pl(page, gw, gh); pl.logging = true;
    std::set<long long> ts;
    for (auto &e : page.events) { long long q = llround(e.t * 50 * 1000); ts.insert((q + 999) / 1000); }
    Json steps = Json::array();
    for (long long q : ts) {
        int sid = pl.surf_id, mv = pl.map_ver, bv = pl.blink_ver;
        pl.advance(q / 50.0 + 1e-9);
        Json st = Json::array();
        st.push(round(q / 50.0 * 1000) / 1000); st.push(pl.surf_id != sid ? 1 : 0);
        st.push(pl.map_ver != mv ? mapj(pl.map) : Json()); st.push(pl.blink_ver != bv ? blinkj(pl.blink) : Json());
        st.push(runs(pl.log)); pl.log.clear();
        steps.push(st);
    }
    if (!pl.done()) {
        int sid = pl.surf_id; pl.advance(0, true);
        Json st = Json::array(); st.push(round(pl.end() * 1000) / 1000); st.push(pl.surf_id != sid ? 1 : 0); st.push(mapj(pl.map)); st.push(blinkj(pl.blink)); st.push(runs(pl.log));
        steps.push(st);
    }
    Json inks = Json::array();
    for (auto &s : pl.inks) {
        Json a = Json::array();
        if (s.k == 'c') { a.push("c"); a.push(rgbj(s.c)); }
        else if (s.k == 'm') { a.push("m"); a.push(((s.a % 16) + 16) % 16); }
        else { a.push("d"); a.push(rgbj(s.c)); a.push(((s.a % 16) + 16) % 16); }
        inks.push(a);
    }
    std::array<Col, 16> dm;
    { Page empty; Player p0(empty); dm = p0.map; }
    Json d = Json::object();
    d["w"] = gw; d["h"] = gh; d["end"] = round(page.end * 1000) / 1000; d["map"] = mapj(dm); d["inks"] = inks; d["steps"] = steps;
    return d;
}

static const char *PAGE1 = R"~(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>)~";
static const char *PAGE2 = R"~(</title>
<style>html,body{margin:0;height:100%;background:#000}
body{display:flex;align-items:center;justify-content:center}
canvas{width:min(100vw,133.333vh);aspect-ratio:4/3;image-rendering:pixelated;display:block;cursor:pointer}</style>
</head><body><canvas id="screen" title="Click to replay"></canvas>
<script>
)~";

int nabts_html_export(const std::vector<Record> &recs, const std::string &out, int gw, int gh, Progress &pr) {
    make_dirs(out);
    std::string js = resource_text("NABTS_PLAYER");
    std::vector<const Record *> pages; for (auto &r : recs) if (r.page) pages.push_back(&r);
    for (size_t k = 0; k < pages.size(); k++) {
        const Record &r = *pages[k];
        std::string html = std::string(PAGE1) + html_escape(record_label(r)) + PAGE2 + js +
                           "\nNabtsPlayer.start(" + page_data(*r.page, gw, gh).dump() + ", document.getElementById('screen'), document.getElementById('screen'), null);\n</script></body></html>\n";
        write_text(path_join(out, record_name(r) + ".html"), html);
        pr.progress(k + 1, pages.size(), "HTML pages");
    }
    return (int)pages.size();
}

void nabts_html_export_file(const std::string &t33, const std::string &out, Progress &pr) {
    Summary s;
    auto recs = read_t33(t33, s, &pr);
    interpret(recs);
    int n = nabts_html_export(recs, out, 256, 200, pr);
    pr.log(fmt("Done: %d pages in ", n) + out);
}
