// trcli — консольный вход в те же функции, что и программа (для проверки и пакетной работы).
//   trcli build <поток.t42|.t34> <папка> [--lines line_pkt.npy --lpf 32 --vbi запись.vbi]
//   trcli export <папка проекта> [--full]
//   trcli srt <папка проекта> [страница]
//   trcli vbi <запись.vbi> [--again]           (как «Открыть .vbi» в программе)
//   trcli decode <запись.vbi> <папка> [--lpf 32] [--cpu]
//   trcli probe <запись.vbi>
//   trcli t33 <поток.t33> [папка]              (PNG и текст каждой записи NABTS)
//   trcli t33html <поток.t33> [папка]
//   trcli lines <запись.vbi> | packets <поток.t42> | vits <запись.vbi>
#include <windows.h>
#include <iostream>
#include "opencl.h"
#include "flac.h"
#include "decode_vbi.h"
#include "nabts.h"
#include "pagebuild.h"
#include "project.h"
#include "tools.h"
#include "teletext.h"
#include "vbi_auto.h"
#include "vbi_probe.h"
#include "vhs_wst.h"
#include "silent_radio.h"
#include "mlse.h"
#include "decode_vbi.h"

static std::string arg(const std::vector<std::string> &a, const std::string &k, const std::string &d = "") {
    for (size_t i = 0; i + 1 < a.size(); i++) if (a[i] == k) return a[i + 1];
    return d;
}
static bool flag(const std::vector<std::string> &a, const std::string &k) { return std::find(a.begin(), a.end(), k) != a.end(); }

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> a;
    for (int i = 1; i < argc; i++) a.push_back(narrow(argv[i]));
    if (a.empty()) { std::cout << "usage: trcli build|export|srt|vbi|decode|probe|t33|t33html|lines|packets|vits ...\n"; return 1; }
    Progress &pr = console_progress();
    // --cpu — считать на процессоре; --gpu N — видеокарта N из списка «trcli devices»
    if (flag(a, "--cpu")) gpu::use(-1);
    else if (!arg(a, "--gpu").empty()) gpu::use(std::stoi(arg(a, "--gpu")));
    try {
        const std::string &cmd = a[0];
        if (cmd == "devices") {
            auto d = gpu::devices(); int b = gpu::best_device();
            for (size_t i = 0; i < d.size(); i++)
                std::cout << i << ": " << d[i].name << " (" << d[i].vendor << fmt(", %zu MB, %d units, %zu threads per group%s)", d[i].mem_mb, d[i].units, d[i].max_group, d[i].integrated ? ", built-in" : "")
                          << ((int)i == b ? "  <- used by default" : "") << "\n";
            if (d.empty()) std::cout << "no graphics card with OpenCL - everything runs on the processor\n";
            return 0;
        }
        if (cmd == "build" && a.size() >= 3) {
            build_project(a[1], a[2], arg(a, "--lines"), std::stoi(arg(a, "--lpf", "32")), arg(a, "--vbi"), pr);
        } else if (cmd == "export" && a.size() >= 2) {
            Project P; if (!P.open(a[1])) throw std::runtime_error("not a project: " + a[1]);
            std::set<std::string> ed; std::string src;
            Pages pages = P.load_state(ed, src);
            std::cout << src << ", " << pages.size() << " pages\n";
            std::cout << P.export_all(pages, flag(a, "--full")) << "\n";
        } else if (cmd == "srt" && a.size() >= 2) {
            Project P; if (!P.open(a[1])) throw std::runtime_error("not a project: " + a[1]);
            std::string page = a.size() > 2 ? a[2] : "";
            if (page.empty()) { auto c = subtitle_pages(P.stream); page = c.empty() ? "888" : c[0].first; }
            auto r = P.export_srt(page);
            std::cout << "page " << page << ": " << r.second << " captions -> " << r.first << "\n";
        } else if ((cmd == "squash" || cmd == "words") && a.size() >= 2) {
            Project P; if (!P.open(a[1])) throw std::runtime_error("not a project: " + a[1]);
            std::set<std::string> ed; std::string src;
            Pages pages = P.load_state(ed, src);
            if (cmd == "words" || flag(a, "--words")) {
                auto fx = P.restore_words(pages);
                int n = 0; for (auto &f : fx) { n += f.n; std::cout << f.page << ": " << f.from << " -> " << f.to << (f.n > 1 ? fmt(" (%d)", f.n) : "") << "\n"; }
                std::cout << n << " words restored on " << std::set<std::string>([&] { std::set<std::string> s; for (auto &f : fx) s.insert(f.page); return s; }()).size() << " pages\n";
                if (flag(a, "--apply")) { for (auto &f : fx) ed.insert(f.page); P.save_state(pages, ed); std::cout << "saved\n"; }
            }
            if (cmd == "squash") std::cout << P.squash(pages) << "\n";
            if (flag(a, "--titles")) for (auto &kv : pages) std::cout << kv.first << "  " << P.page_title(kv.second, kv.first) << "\n";
            std::cout << P.service_line() << "\n";
        } else if (cmd == "flac" && a.size() >= 3) {
            flac_to_raw(a[1], a[2], pr);
        } else if (cmd == "service" && a.size() >= 2) {
            std::cout << service_info(read_t42(a[1])).dump() << "\n";
        } else if (cmd == "vbi" && a.size() >= 2) {
            vbi_auto(a[1], flag(a, "--again"), pr);
        } else if (cmd == "decode" && a.size() >= 3) {
            decode_vbi_project(a[1], a[2], std::stoi(arg(a, "--lpf", "32")), !flag(a, "--cpu"), !flag(a, "--no-repair"), pr);
        } else if (cmd == "vhs" && a.size() >= 3) {
            std::vector<int> rows; for (auto &x : split(arg(a, "--rows", "9,12,13,14,15,25,28,29,30,31"), ',')) rows.push_back(std::stoi(x));
            vhs_wst_project(a[1], a[2], rows, pr);
        } else if (cmd == "vhsdbg" && a.size() >= 3) {
            Json pj = load_json(a[2]);
            std::vector<double> prior; for (auto &v : pj["prior"].a) for (auto &x : v.a) prior.push_back(x.num());
            double off = pj["off"].num(); int n0 = pj["n"].integer();
            MappedFile mf(a[1]);
            int rows[5] = {25, 28, 29, 30, 31};
            std::vector<std::pair<int,int>> sel;
            for (int f = 0; f < 3000; f++) for (int r : rows) { const u8 *l = mf.data() + (uint64_t)f * 65536 + r * 2048; if (line_std(l + 100, 1800) > 12) sel.push_back({f, r}); }
            MlseModel m; m.spb = 35468950.0 / 6937500.0; m.pre[0] = 0x55; m.pre[1] = 0x55; m.pre[2] = 0x27; m.nbytes = 42; m.L = 6; m.R = 5; m.ntop = 2; m.refits = 1;
            m.good = [](const u8 *p) { return ham::dec[p[0]] >= 0 && ham::dec[p[1]] >= 0; };
            MlseBatch bd(m, prior, n0, !flag(a, "--cpu"), pr);
            size_t n = 1456; std::vector<float> buf(n * 2044); std::vector<const float *> rp(n);
            for (size_t i = 0; i < n; i++) { const u8 *l = mf.data() + (uint64_t)sel[i].first * 65536 + sel[i].second * 2048; for (int q = 0; q < 2044; q++) buf[i * 2044 + q] = l[q]; rp[i] = &buf[i * 2044]; }
            auto res = bd.decode(rp, 2044, std::max(0.0, off - 8), off + 8, nullptr, 0.5);
            int g = 0; for (auto &r : res) g += r.good;
            std::cout << "good " << g << " of " << n << "\noffs"; for (int i = 0; i < 8; i++) std::cout << " " << res[i].off;
            std::cout << "\nmets"; for (int i = 0; i < 8; i++) std::cout << " " << res[i].met;
            std::cout << "\ndata0"; for (int i = 0; i < 8; i++) std::cout << " " << (int)res[0].data[i]; std::cout << "\n";
        } else if (cmd == "srbytes" && a.size() >= 3) {
            sr_save(a[1], a[2], read_file(a[1]), 21, pr);
        } else if (cmd == "ts" && a.size() >= 2) {
            ts_to_t42(a[1], a.size() > 2 ? a[2] : stem_path(a[1]) + ".t42", pr);
        } else if (cmd == "probe" && a.size() >= 2) {
            Json r = vbi_probe(a[1], pr);
            if (!arg(a, "--json").empty()) save_json(arg(a, "--json"), r, 1);
        } else if (cmd == "t33" && a.size() >= 2) {
            nabts_dump(a[1], a.size() > 2 ? a[2] : stem_path(a[1]) + "_nabts", pr);
        } else if (cmd == "t33html" && a.size() >= 2) {
            nabts_html_export_file(a[1], a.size() > 2 ? a[2] : stem_path(a[1]) + "_nabts_html", pr);
        } else if (cmd == "lines" && a.size() >= 2) {
            vbi_lines_survey(a[1], pr);
        } else if (cmd == "packets" && a.size() >= 2) {
            for (size_t i = 1; i < a.size(); i++) service_packets_survey(a[i], pr);
        } else if (cmd == "vits" && a.size() >= 2) {
            vits_analyse_file(a[1], pr);
        } else {
            std::cout << "unknown command\n"; return 1;
        }
    } catch (Cancelled &) {
        std::cout << "cancelled\n"; return 3;
    } catch (std::exception &e) {
        std::cout << e.what() << "\n"; return 2;
    }
    return 0;
}
