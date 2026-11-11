#include "legacy_bridge.h"
#include "legacy/pre.h"
namespace legacy {
#include "legacy/src/vbi_auto.h"
#include "legacy/src/vbi_probe.h"
}

static const std::pair<const char*, VbiFormat> kLegacyFormats[] = {
        {"bt8x8-pal", VbiFormat::BttvPal}, {"bt8x8-ntsc", VbiFormat::Bt8x8}, {"cx88-pal", VbiFormat::Cx88Pal},
        {"cx88-ntsc", VbiFormat::Cx88}, {"saa7131", VbiFormat::Saa713xPal}, {"d27-pal", VbiFormat::IvtvPal},
        {"cx23885", VbiFormat::Cx23885}, {"em28xx-pal", VbiFormat::Em28xxPal}, {"em28xx-ntsc", VbiFormat::Em28xx},
        {"4fsc16", VbiFormat::Tbc}, {"4fsc16-pal-vbi", VbiFormat::TbcPal}, {"tbc-pal", VbiFormat::TbcFullPal},
        {"tbc-ntsc", VbiFormat::TbcFull}};

// the old engine's name of one of our layouts: only where its records are the same (d27-pal = 1440 x 36 from line 6 =
// cx23885-pal, not the 1444-sample ivtv-pal); "" = none
static std::string legacyName(VbiFormat f)
{
    if (f == VbiFormat::Cx23885Pal) return "d27-pal";
    if (f == VbiFormat::IvtvPal) return "";
    for (auto& m : kLegacyFormats)
        if (m.second == f) return m.first;
    return "";
}

VbiFormat legacyDetectFormat(const std::string& path, std::string* name)
{
    std::string n;
    try { n = legacy::detect_format(path).first; } catch (...) {}
    if (name) *name = n;
    for (auto& m : kLegacyFormats)
        if (n == m.first) return m.second;
    return VbiFormat::Unknown;
}

namespace legacy { extern std::map<int, std::string> g_force_rows; extern std::string g_force_format; namespace gpu { extern bool g_allowed; } }

std::string legacyReportDir(const std::string& recording) { return legacy::vbi_report_dir(recording); }

bool legacyRun(const std::string& recording, bool again, const std::function<void(const std::string&)>& step,
               const std::function<void(const std::string&)>& log, std::atomic<bool>* cancel, LegacyReport& out, std::string& err,
               const std::map<int, std::string>& forceRows, bool gpu, VbiFormat format)
{
    legacy::g_force_rows = forceRows;
    // the layout ours decided on (chosen by hand in VBI lines, named by the recording's .info): the engine reads the
    // file the same way, its rows are ours (detecting again took cx88-pal for a bttv-pal file and converted it)
    legacy::g_force_format = legacyName(format);
    legacy::gpu::g_allowed = gpu;
    legacy::Progress pr;
    pr.on_step = [&](const std::string& s) { if (step) step(s); };
    pr.on_log = [&](const std::string& s) { if (log) log(s); };
    pr.on_progress = [&](long long, long long, const std::string&) { if (cancel && *cancel) pr.cancel = true; };
    legacy::Json r;
    try {
        r = legacy::vbi_auto(recording, again, pr);
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
    out = LegacyReport{};
    out.dir = legacy::vbi_report_dir(recording);
    out.format = r["format"].is_null() ? "" : r["format"].str();
    for (auto& L : r["probe"]["lines"].a) {
        LegacyLine l;
        l.row = L["row"].integer();
        l.tvLine = L["tv_line"].integer();
        l.kind = L.get_str("kind");
        l.signal = L["signal"].num(0);
        out.lines.push_back(l);
    }
    for (auto& x : r["results"].a) out.results.push_back({x.get_str("service"), x.get_str("kind"), x.get_str("path"), x.get_str("note")});
    for (auto& v : r["vits_text"].a) out.vits.push_back(v.str());
    return true;
}
