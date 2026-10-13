#include "ttpages.h"
#include "json.h"
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

bool ttBuild(const Bytes& stream, bool is525, const std::string& dir, const std::string& sourceName, TtResult& out,
             const std::function<void(const std::string&)>& log, std::string& err)
{
    out = TtResult{};
    try {
        make_dirs(dir);
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

std::string ttClean(const std::string& project, Progress& pr, std::string& note)
{
    Project P;
    if (!P.open(project)) throw std::runtime_error("no teletext project in " + project);
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
    std::string out = path_join(P.outdir, "clean");
    build_project(path_join(P.outdir, "clean.t42"), out, "", 32, "", pr);
    std::string pj = path_join(out, "project.json");
    Json mj = load_json(pj, Json::object());
    mj["charset"] = P.charset;
    mj["charset2"] = P.charset2.empty() ? Json() : Json(P.charset2);
    mj["cleaned"] = true;
    save_json(pj, mj, 1);
    return out;
}
