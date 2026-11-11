#include "ttpages.h"
#include "ttsquash.h"
#include "json.h"
#include "teletext.h"
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

std::string ttWorkDir(const std::string& recording)
{
#ifdef _WIN32
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::string t = narrow(tmp);
#else
    std::string t = U(fs::temp_directory_path());
#endif
    return path_join(path_join(t, "TeletextRescue"), basename(stem_path(recording)) + "_teletext");
}

std::string ttPagesDir(const std::string& recording)
{
    if (!recording.empty()) {
        std::string beside = path_join(dirname(recording), basename(stem_path(recording)) + "_teletext");
        std::error_code ec;
        fs::create_directories(P(beside), ec);
        if (is_dir(beside)) return beside;
    }
    return ttWorkDir(recording);
}

bool ttBuild(const Bytes& stream, bool is525, const std::string& dir0, const std::string& sourceName, TtResult& out,
             const std::function<void(const std::string&)>& log, std::string& err)
{
    out = TtResult{};
    // во временную папку не пишем никогда: страницы строятся рядом с записью (иначе человек их не находит)
    std::string dir = dir0;
#ifdef _WIN32
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::string t = narrow(tmp);
        if (!t.empty() && dir.size() >= t.size() && dir.compare(0, t.size(), t) == 0) dir = ttPagesDir(sourceName);
    }
#endif
    try {
        {
            // причина отказа видна человеку, а не «cannot write» без объяснения
            std::error_code ec;
            fs::create_directories(P(dir), ec);
            if (!is_dir(dir)) throw std::runtime_error("cannot create the folder " + dir + ": " +
                (ec ? ec.message() : std::string("Windows did not let it be made")) + ". Choose another folder and repeat.");
        }
        // the builder takes a file; the name keeps the recording's name for the pages
        std::string f = path_join(dir, basename(stem_path(sourceName)) + (is525 ? ".t34" : ".t42"));
        write_file(f, stream);
        Progress pr;
        pr.on_log = [&](const std::string& m) { if (log) log("teletext: " + m); };
        pr.on_step = [&](const std::string& m) { if (log) log("teletext: " + m); };
        build_project(f, dir, "", 0, "", pr);
        Project P;
        if (!P.open(dir)) { err = "the teletext project was not written"; return false; }
        // 525-line teletext was North American (English): noisy tape can make the
        // guess land on Cyrillic, which never ran on 525 lines
        if (is525 && P.charset != "latin") {
            P.set_charset("latin");
            if (log) log("teletext: character set Latin (525-line teletext was North American)");
        }
        std::set<std::string> edited;
        std::string src;
        Pages pages = P.load_state(edited, src);
        out.project = dir;
        out.packets = stream.size() / (is525 ? 34 : 42);
        out.pages = (int)pages.size();
        for (auto& kv : pages) out.subpages += (int)kv.second.versions.size();
        out.service = P.service_line();
        out.system = is525 ? "525-line WST" : "625-line WST (DVB)";
        return true;
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
}

std::string ttRecheckCharset(const std::string& project)
{
    if (project.empty()) return "";
    std::string pjPath = path_join(project, "project.json");
    Json pj = load_json(pjPath, Json());
    if (!pj.is_obj()) return "";
    std::string was = pj.get_str("charset");
    if (was != "latin" && was != "cyr2") return "";   // the only two the old guess gives; a set chosen by hand stays
    Json ex = load_json(path_join(project, "extras.json"), Json::object());
    if (!ex["_meta"].get_str("g0").empty()) return "";
    Json pages = load_json(path_join(project, "pages.json"), Json());
    if (!pages.is_obj()) return "";
    std::vector<std::array<u8, 40>> keep;
    for (auto& kv : pages.o) {
        if (!kv.second.is_arr() || kv.second.a.empty()) continue;
        for (auto& r : kv.second.a[0]["rows"].o) {
            if (r.first == "0" || !r.second.is_arr()) continue;
            std::array<u8, 40> b{};
            for (size_t i = 0; i < 40 && i < r.second.a.size(); i++) b[i] = (u8)r.second.a[i].num();
            keep.push_back(b);
        }
    }
    std::vector<const u8*> rows;
    for (auto& b : keep) rows.push_back(b.data());
    std::string cs = guess_charset(rows, (int)ex["_meta"].get_num("national", 0));
    if (cs == was) return "";
    pj["charset"] = cs;
    save_json(pjPath, pj, 1);
    return cs;
}

// The folder for clean.t42: made now, and said plainly when Windows does not let it be made — the copy
// and the build would otherwise fail with "No such file or directory" at whichever file comes first
static void need_dir(const std::string &dir)
{
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    if (!is_dir(dir)) {
        std::string why = ec ? ec.message() : std::string("unknown error");
        throw std::runtime_error("cannot create the folder " + dir + ": " + why +
                                 "\n\nChoose another folder (for example on the Desktop or in Documents) and repeat.");
    }
}

// Windows не даёт писать в выбранную папку (чаще всего это «Защита от подмены» в Безопасности Windows):
// тогда берём папку с тем же именем рядом с записью — человек видит, куда легло, и работа не пропадает
static bool ensure_dest(std::string& dest, const std::string& beside, std::string& why)
{
    std::error_code ec;
    fs::create_directories(P(dest), ec);
    if (is_dir(dest)) return true;
    why = ec ? ec.message() : std::string("Windows did not let it be made");
    std::string alt = path_join(beside, basename(dest));
    std::error_code ec2;
    fs::create_directories(P(alt), ec2);
    if (is_dir(alt)) { dest = alt; return true; }
    return false;
}

std::string ttClean(const std::string& project, Progress& pr, std::string& note, const std::string& dest, const std::string& squash)
{
    Project P;
    if (!P.open(project)) throw std::runtime_error("no teletext project in " + project);
    // папку результата проверяем сразу, а не после получаса сборки
    std::string destDir = dest, why;
    if (!destDir.empty() && !ensure_dest(destDir, dirname(project), why))
        throw std::runtime_error("cannot create the folder " + dest + ": " + why + ". Choose another folder (for example on the Desktop) and repeat.");
    if (!destDir.empty() && destDir != dest)
        note += "the folder " + dest + " is closed for writing (" + why + "), the result went to " + destDir;
    if (!squash.empty()) {   // VHSTTX squash over the stream as received
        SquashOptions so;
        if (!squashOptionsByName(squash, so)) throw std::runtime_error("unknown squash mode " + squash);
        pr.step("squash (VHSTTX " + squash + ")");
        std::string raw = read_text(P.stream);
        std::vector<uint8_t> in(raw.begin(), raw.end()), outT = ttSquash(in, so, note);
        std::string t42 = path_join(P.outdir, "clean.t42");
        write_atomic(t42, std::string(outT.begin(), outT.end()));
        std::string out = destDir.empty() ? path_join(P.outdir, "clean") : destDir;
        if (!destDir.empty()) {
            write_atomic(path_join(destDir, "clean.t42"), std::string(outT.begin(), outT.end()));
            note += "; saved in " + destDir;
        }
        pr.step("building the cleaned pages");
        build_project(t42, out, "", 32, "", pr);
        std::string pj = path_join(out, "project.json");
        Json mj = load_json(pj, Json::object());
        mj["charset"] = P.charset;
        mj["charset2"] = P.charset2.empty() ? Json() : Json(P.charset2);
        mj["cleaned"] = true;
        save_json(pj, mj, 1);
        return out;
    }
    std::set<std::string> edited;
    std::string src;
    Pages pages = P.load_state(edited, src);
    pr.step("restoring damaged words");
    auto fx = P.restore_words(pages, nullptr, true);
    int n = 0;
    for (auto& f : fx) n += f.n;
    pr.step("assembling one copy of every subpage");
    note = P.squash(pages) + (n ? fmt("; %d damaged words restored", n) : "");
    pr.step("building the cleaned pages");
    std::string out = destDir.empty() ? path_join(P.outdir, "clean") : destDir, t42 = path_join(P.outdir, "clean.t42");
    if (!destDir.empty()) {
        if (!exists(t42)) throw std::runtime_error("the cleaned stream was not written: " + t42);
        std::error_code ec;
        fs::copy_file(std::filesystem::u8path(t42), std::filesystem::u8path(path_join(destDir, "clean.t42")),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("cannot write " + path_join(destDir, "clean.t42") + ": " + ec.message());
        note += "; saved in " + destDir;
    }
    build_project(t42, out, "", 32, "", pr);
    std::string pj = path_join(out, "project.json");
    Json mj = load_json(pj, Json::object());
    mj["charset"] = P.charset;
    mj["charset2"] = P.charset2.empty() ? Json() : Json(P.charset2);
    mj["cleaned"] = true;
    save_json(pj, mj, 1);
    return out;
}
