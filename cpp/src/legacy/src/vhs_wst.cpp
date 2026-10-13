#include "vhs_wst.h"
#include <thread>
#include "decode_vbi.h"
#include "mlse.h"
#include "pagebuild.h"

static bool hamclean(u8 x) { return ham::dec[x] >= 0; }

static MlseModel vhs_model() {
    MlseModel m;
    m.spb = 35468950.0 / 6937500.0; m.pre[0] = 0x55; m.pre[1] = 0x55; m.pre[2] = 0x27; m.nbytes = 42;
    m.L = 6; m.R = 5; m.ntop = 2; m.refits = 1;
    m.good = [](const u8 *p) { return hamclean(p[0]) && hamclean(p[1]); };
    // адрес и заголовок — только кодовые слова Хэмминга, текст — только нечётная чётность
    m.code_kind = 1;      // то же правило в REFINE_KERNEL (mlse.cpp)
    m.code = [](const u8 *p, int i) -> int {
        if (i < 2) return 1;
        int a = ham::dec[p[0]], b = ham::dec[p[1]];
        if (a < 0 || b < 0) return 0;
        int row = (a >> 3) | (b << 1);
        if (row == 0) return i < 10 ? 1 : 2;
        if (row <= 25) return 2;
        if (row == 27) return i < 40 ? 1 : 0;
        if (row >= 26 && row <= 29) return i == 2 ? 1 : 0;
        return 0;
    };
    return m;
}

static bool has_data(const u8 *y) { return line_std(y + 100, 1800) > 12; }

// ---------------------------------------------------------------- сборка по содержимому
namespace {
using R40 = std::array<u8, 40>;
struct Clu { std::vector<R40> rows; R40 v; std::array<bool, 40> g; };
void vote(const std::vector<R40> &rs, size_t from, size_t to, R40 &out, std::array<bool, 40> &good) {
    for (int k = 0; k < 40; k++) {
        int cnt[128] = {0}; bool any = false;
        for (size_t i = from; i < to; i++) { u8 c = rs[i][k]; if (odd_parity(c)) { cnt[c & 0x7F]++; any = true; } }
        if (any) { int b = 0; for (int q = 1; q < 128; q++) if (cnt[q] > cnt[b]) b = q; out[k] = (u8)b; good[k] = true; }
        else { out[k] = 0x20; good[k] = false; }
    }
}
double sim(const R40 &v, const std::array<bool, 40> &g, const u8 *b) {
    int m = 0, eq = 0;
    for (int k = 0; k < 40; k++) {
        u8 bb = b[k] & 0x7F;
        if (g[k] && odd_parity(b[k]) && (v[k] != 0x20 || bb != 0x20)) { m++; eq += v[k] == bb; }
    }
    return m >= 8 ? (double)eq / m : 0.0;
}
}

static PagesBuild assemble(const std::vector<std::pair<int, std::array<u8, 42>>> &packets, double row_th = 0.5, double page_th = 0.6) {
    std::map<std::pair<int, int>, std::vector<Clu>> rcl;
    struct Seq { int mag, y, k, f; R40 hb; };
    std::vector<Seq> seq;
    for (auto &fp : packets) {
        const u8 *p = fp.second.data();
        int a = ham::dec[p[0]], b = ham::dec[p[1]];
        int mag = (a & 7) ? (a & 7) : 8, y = (a >> 3) | (b << 1);
        if (y > 24) continue;
        R40 r; memcpy(r.data(), p + 2, 40);
        if (y == 0) { seq.push_back({mag, 0, -1, fp.first, r}); continue; }
        auto &cl = rcl[{mag, y}];
        int best = -1; double bs = 0;
        for (size_t k = 0; k < cl.size(); k++) { double s = sim(cl[k].v, cl[k].g, p + 2); if (s > row_th && (best < 0 || s > bs)) { bs = s; best = (int)k; } }
        int k;
        if (best >= 0) {
            k = best; Clu &c = cl[k]; c.rows.push_back(r);
            if (c.rows.size() <= 60 || c.rows.size() % 10 == 0) vote(c.rows, c.rows.size() > 60 ? c.rows.size() - 60 : 0, c.rows.size(), c.v, c.g);
        } else {
            Clu c; c.rows.push_back(r); vote(c.rows, 0, 1, c.v, c.g); cl.push_back(c); k = (int)cl.size() - 1;
        }
        seq.push_back({mag, y, k, fp.first, {}});
    }
    for (auto &kv : rcl) for (auto &c : kv.second) vote(c.rows, 0, std::min<size_t>(200, c.rows.size()), c.v, c.g);
    struct PP { int mag, f; std::vector<R40> hdr; std::map<int, int> r; };
    std::vector<PP> pp; std::map<int, int> cur;
    for (auto &s : seq) {
        if (s.y == 0) { PP p; p.mag = s.mag; p.f = s.f; p.hdr.push_back(s.hb); pp.push_back(p); cur[s.mag] = (int)pp.size() - 1; }
        else if (cur.count(s.mag)) pp[cur[s.mag]].r.emplace(s.y, s.k);
    }
    auto nonempty = [&](int mag, int y, int k) {
        auto &v = rcl[{mag, y}][k].v; int n = 0;
        for (u8 c : v) n += (c != 0x20 && c >= 0x20);
        return n >= 6;
    };
    struct G { int mag; std::map<int, std::map<int, int>> cnt; std::vector<int> m; };
    std::vector<G> groups;
    for (size_t i = 0; i < pp.size(); i++) {
        auto &p = pp[i];
        std::map<int, int> keys;
        for (auto &yk : p.r) if (nonempty(p.mag, yk.first, yk.second)) keys[yk.first] = yk.second;
        if (keys.size() < 3) continue;
        int best = -1; double bs = 0;
        for (size_t gi = 0; gi < groups.size(); gi++) {
            auto &g = groups[gi];
            if (g.mag != p.mag) continue;
            std::map<int, int> proto;
            for (auto &yc : g.cnt) { int bk = -1, bn = -1; for (auto &kn : yc.second) if (kn.second > bn) { bn = kn.second; bk = kn.first; } proto[yc.first] = bk; }
            int common = 0, same = 0;
            for (auto &yk : keys) if (proto.count(yk.first)) { common++; same += proto[yk.first] == yk.second; }
            if (common < 3) continue;
            double sc = (double)same / common;
            if (sc >= page_th && (best < 0 || sc > bs)) { bs = sc; best = (int)gi; }
        }
        if (best < 0) { G g; g.mag = p.mag; groups.push_back(g); best = (int)groups.size() - 1; }
        G &g = groups[best];
        g.m.push_back((int)i);
        for (auto &yk : keys) g.cnt[yk.first][yk.second]++;
    }
    std::vector<G *> gs;
    for (auto &g : groups) if (g.m.size() >= 2) gs.push_back(&g);
    std::stable_sort(gs.begin(), gs.end(), [&](G *a, G *b) { return a->mag != b->mag ? a->mag < b->mag : pp[a->m[0]].f < pp[b->m[0]].f; });
    PagesBuild pages; std::map<int, int> seqn;
    for (G *g : gs) {
        int mag = g->mag;
        if (++seqn[mag] > 99) continue;
        std::string pid = fmt("%d%02d", mag, seqn[mag]);
        Version v;
        for (auto &yc : g->cnt) {
            int bk = -1, bn = -1; for (auto &kn : yc.second) if (kn.second > bn) { bn = kn.second; bk = kn.first; }
            auto &vv = rcl[{mag, yc.first}][bk].v;
            Row r; std::copy(vv.begin(), vv.end(), r.begin()); v.rows[yc.first] = r;
        }
        std::vector<R40> hs;
        for (int i : g->m) hs.insert(hs.end(), pp[i].hdr.begin(), pp[i].hdr.end());
        R40 hv; std::array<bool, 40> hg; vote(hs, 0, hs.size(), hv, hg);
        std::string lab = fmt("P%s  ? copies %zu", pid.c_str(), g->m.size());
        lab.resize(32, ' ');
        Row h; for (int k = 0; k < 32; k++) h[k] = (u8)lab[k];
        for (int k = 32; k < 40; k++) h[k] = hv[k];
        v.rows[0] = h;
        v.t = round(pp[g->m[0]].f / 25.0 * 100) / 100; v.n = (int)g->m.size();
        pages[pid] = {v};
    }
    return pages;
}

void vhs_wst_project(const std::string &vbi_in, const std::string &out_in, const std::vector<int> &rows, Progress &pr) {
    std::string src = abspath(vbi_in), out = abspath(out_in);
    make_dirs(out);
    MappedFile mf(src);
    int N = (int)(mf.size() / 65536);
    auto line = [&](int f, int r) { return mf.data() + (uint64_t)f * 65536 + (uint64_t)r * 2048; };
    std::vector<std::vector<int>> fields(2);
    for (int r : rows) fields[r >= 16].push_back(r);
    MlseModel M = vhs_model();
    pr.step("Tape recording: learning the channel of each field");
    struct WS { std::unique_ptr<MlseDecoder> dec; double off; bool ok; };
    std::vector<WS> ws(2);
    // поля учатся независимо — оба сразу (второе в отдельном потоке, прогресс показывает первое)
    auto learn = [&](int k, bool report) {
        if (fields[k].empty()) { ws[k].ok = false; return; }
        std::vector<float> y(2044);
        ws[k].dec.reset(new MlseDecoder(M)); ws[k].off = -1;
        int n = 0;
        for (int f = 0; f < N; f += 7) {
            for (int r : fields[k]) {
                const u8 *l = line(f, r);
                if (!has_data(l)) continue;
                for (int q = 0; q < 2044; q++) y[q] = l[q];
                double lo = 60.0, hi = 140.0;          // окно задаётся до цикла и не сужается
                auto res = ws[k].dec->decode(y.data(), 2044, lo, hi, 0.5); n++;
                if (res.good) ws[k].off = res.off;
            }
            if (report) pr.progress(std::min(n, 300), 300, "learning");
            if (ws[k].dec->n > 150 || n >= 300) break;
        }
        ws[k].ok = ws[k].dec->n >= 30;
    };
    {
        std::thread t2([&] { learn(1, fields[0].empty()); });
        learn(0, true);
        t2.join();
    }
    for (int k = 0; k < 2; k++) if (ws[k].dec) {
        pr.log(fmt("field %d: %d packets with a valid address while learning", k + 1, ws[k].dec->n)); if (getenv("TR_DEBUG")) pr.log(fmt("warm off %.2f prior0 %.5f %.5f", ws[k].off, ws[k].dec->prior.empty() ? 0 : ws[k].dec->prior[0], ws[k].dec->prior.empty() ? 0 : ws[k].dec->prior[1]));
    }
    int gk = ws[0].ok ? 0 : ws[1].ok ? 1 : -1;
    if (gk < 0) throw std::runtime_error("The tape decoder could not read this recording either.");
    std::vector<std::tuple<int, int, std::array<u8, 42>>> pk;
    for (int k = 0; k < 2; k++) {
        if (fields[k].empty()) continue;
        std::vector<double> prior = ws[k].ok ? ws[k].dec->prior : ws[gk].dec->prior;
        int nprior = ws[k].ok ? ws[k].dec->n : 20;
        double off = ws[k].ok ? ws[k].off : ws[gk].off;
        std::string names;
        for (int r : fields[k]) names += (names.empty() ? "" : ", ") + std::to_string(r < 16 ? 7 + r : 320 + r - 16);
        pr.step(fmt("Reading field %d on lines %s", k + 1, names.c_str()));
        MlseBatch bd(M, prior, nprior, true, pr);
        std::vector<std::pair<int, int>> sel;
        for (int f = 0; f < N; f++) for (int r : fields[k]) if (has_data(line(f, r))) sel.push_back({f, r});
        double lo = std::max(0.0, off - 8), hi = off + 8;
        size_t good = 0;
        std::vector<float> buf;
        for (size_t b = 0; b < sel.size(); b += bd.batch()) {
            size_t n = std::min(bd.batch(), sel.size() - b);
            buf.resize(n * 2044);
            std::vector<const float *> rp(n);
            for (size_t i = 0; i < n; i++) { const u8 *l = line(sel[b + i].first, sel[b + i].second); for (int q = 0; q < 2044; q++) buf[i * 2044 + q] = l[q]; rp[i] = &buf[i * 2044]; }
            auto res = bd.decode(rp, 2044, lo, hi, nullptr, 0.5);
            std::vector<double> go;
            // все строки поля: адрес и заголовок перерешены среди кодовых слов Хэмминга;
            // поле, где чистых адресов почти нет, ниже выбрасывается целиком
            for (size_t i = 0; i < n; i++) {
                if (res[i].good) { go.push_back(res[i].off); good++; }
                std::array<u8, 42> d; memcpy(d.data(), res[i].data.data(), 42);
                pk.emplace_back(sel[b + i].first, sel[b + i].second, d);
            }
            if (!go.empty()) { double c = median(go); lo = std::max(0.0, c - 8); hi = c + 8; }
            pr.progress(b + n, sel.size());
        }
        double share = sel.empty() ? 0 : (double)good / sel.size();
        pr.log(fmt("field %d: %zu lines, %zu with a valid address (%.0f%%)", k + 1, sel.size(), good, share * 100));
        if (share < 0.05) {
            pr.log(fmt("field %d is unreadable (worn or dirty video head?) \xE2\x80\x94 skipped", k + 1));
            pk.erase(std::remove_if(pk.begin(), pk.end(), [&](auto &t) { return (std::get<1>(t) >= 16) == (k == 1); }), pk.end());
        }
    }
    std::sort(pk.begin(), pk.end(), [](auto &a, auto &b) { return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b); });
    std::vector<Packet42> st;
    std::vector<std::pair<int, std::array<u8, 42>>> fp;
    for (auto &t : pk) { Packet42 p; memcpy(p.data(), std::get<2>(t).data(), 42); st.push_back(p); fp.push_back({std::get<0>(t), std::get<2>(t)}); }
    write_t42(path_join(out, "stream.t42"), st);
    // номера страниц читаются (несколько номеров повторяются и дают большинство заголовков) — обычная сборка по номерам
    std::map<int, int> hdr; int nh = 0;
    for (auto &p : st) {
        int a = ham::dec[p[0]], b = ham::dec[p[1]], u = ham::dec[p[2]], t = ham::dec[p[3]];
        if (a < 0 || b < 0 || ((a >> 3) | (b << 1)) != 0 || u < 0 || t < 0) continue;
        nh++; hdr[((a & 7) << 8) | (t << 4) | u]++;
    }
    int rep = 0, npg = 0;
    for (auto &kv : hdr) if (kv.second >= 3) { rep += kv.second; npg++; }
    if (npg >= 2 && rep >= 0.6 * nh) {
        pr.log(fmt("page numbers are readable: %d pages in %d headers", npg, nh));
        LineMap lp; lp.frames = N; lp.lpf = 32; lp.v.assign((size_t)N * 32, -1);
        for (size_t i = 0; i < pk.size(); i++) lp.v[(size_t)std::get<0>(pk[i]) * 32 + std::get<1>(pk[i])] = (int32_t)i;
        std::string lines = path_join(out, "line_pkt.npy");
        save_npy_i32(lines, lp);
        build_project(path_join(out, "stream.t42"), out, lines, 32, src, pr);
        Json pj = load_json(path_join(out, "project.json"), Json::object());
        pj["tape_decoder"] = true;
        pj["note"] = "read from tape with the wide-channel decoder: addresses and headers restricted to Hamming codewords, text to odd parity";
        save_json(path_join(out, "project.json"), pj, 1);
        return;
    }
    pr.step("Assembling pages by content");
    PagesBuild pages = assemble(fp);
    std::vector<const u8 *> rows_txt;
    for (auto &kv : pages) for (auto &v : kv.second) for (auto &r : v.rows) rows_txt.push_back(r.second.data());
    std::string cs = rows_txt.empty() ? "latin" : guess_charset(rows_txt);
    save_json(path_join(out, "pages.json"), build_to_json(pages));
    if (!exists(path_join(out, "extras.json"))) write_text(path_join(out, "extras.json"), "{}");
    Json pj = Json::object();
    pj["source"] = src; pj["stream"] = path_join(out, "stream.t42"); pj["lines"] = Json(); pj["field_skipping"] = false;
    pj["break_rates"] = Json(); pj["charset"] = cs; pj["tape_decoder"] = true;
    pj["note"] = "page numbers were not readable; pages are numbered in order within each magazine";
    save_json(path_join(out, "project.json"), pj, 1);
    std::string name = cs; for (auto &c : CHARSET_NAMES) if (c.first == cs) name = c.second;
    pr.log(fmt("%zu pages assembled (%s); page numbers were not readable \xE2\x80\x94 numbered in order within each magazine", pages.size(), name.c_str()));
}
