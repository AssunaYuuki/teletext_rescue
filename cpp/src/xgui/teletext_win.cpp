// Teletext window (as src/gui_teletext.cpp): the page editor of the original Teletext Rescue.
//   left  - page list with search and sortable columns;
//   right - tabs "Page" (navigation, versions, concealed text, complete page, as on a TV, control codes,
//           the SAA5050 screen with a cursor: click and type, arrows, Backspace / Delete, Enter,
//           PageUp / PageDown; FLOF keys, row / version / page actions, colour palettes and attribute
//           codes) and "Tape quality" (charts and table per minute);
//   menus - File (save, export, complete pages, subtitles, PNG, folder), Character set, Tools (clean
//           stream, restore words). Ctrl+S saves, Ctrl+Z undoes.
#include "draw.h"
#include "pagebuild.h"
#include "png.h"
#include "project.h"
#include "teletext.h"
#include "ttpages.h"
#include <cmath>
#include <set>

namespace xg {
namespace {
const ImU32 KEYC[5] = {rgb(0xdd, 0x22, 0x22), rgb(0x22, 0xaa, 0x22), rgb(0xcc, 0xcc, 0x22), rgb(0x22, 0xcc, 0xcc), rgb(0x99, 0x99, 0x99)};
const ImU32 TT[8] = {rgb(0, 0, 0), rgb(255, 0, 0), rgb(0, 255, 0), rgb(255, 255, 0), rgb(0, 0, 255), rgb(255, 0, 255), rgb(0, 255, 255), rgb(255, 255, 255)};
const std::vector<std::pair<std::string, std::string>> CS2 = {{"", "none"}, {"latin", "Latin"}, {"cyr2", "Cyrillic (Russian)"}, {"cyr1", "Cyrillic (Serbian)"}, {"cyr3", "Cyrillic (Ukrainian)"}};

struct TtWin : Win {
    ResultPtr R;
    std::string projDir;
    Project P;
    bool has_project = false;
    Pages pages;
    std::vector<std::string> ids, list_ids;
    std::string cur, sort_key = "p", msg;
    size_t ver = 0;
    std::set<std::string> edited;
    bool dirty = false;
    std::vector<std::pair<std::string, Page>> undo;
    int cursor_r = 1, cursor_c = 0;
    bool flash_on = true, last_flash = false, last_box = false;
    double flash_at = 0;
    std::map<std::string, std::vector<Version>> full_cache;
    bool reveal = false, full = false, tv = false, codes = false, quality_tab = false, cleaned = false;
    bool follow = false, screen_focus = false, redraw = true;
    char search[64] = "", pnum[8] = "", keys[5][8] = {};
    std::string kpage;
    Json quality;
    Tex tex;

    void status(const std::string &t) { msg = t; }
    bool load(const std::string &path)
    {
        Project np;
        if (!np.open(path)) { status("the teletext project is missing: " + path); return false; }
        Pages pg;
        std::set<std::string> ed;
        std::string src;
        try { pg = np.load_state(ed, src); if (pg.empty()) throw std::runtime_error("the project has no pages"); }
        catch (std::exception &e) { status(e.what()); return false; }
        projDir = path;
        P = np; has_project = true; pages = pg; edited = ed; dirty = false; undo.clear(); full_cache.clear();
        ids.clear();
        for (auto &kv : pages) ids.push_back(kv.first);
        cur = pages.count("100") ? "100" : ids[0];
        ver = 0; cursor_r = 1; cursor_c = 0; kpage.clear();
        cleaned = path != R->teletext.project;
        fill_list();
        open_page(cur);
        quality = load_json(P.quality_path);
        std::string sl = P.service_line();
        status(fmt("%zu pages \xC2\xB7 ", ids.size()) + P.system + (sl.empty() ? "" : " \xC2\xB7 " + sl));
        return true;
    }
    void fill_list()
    {
        std::string f = lower(strip(search));
        struct It { std::string p, title, note; int tx; size_t nv; };
        std::vector<It> items;
        for (auto &p : ids) {
            Page &pg = pages[p];
            std::string title = P.page_title(pg, p);
            if (!f.empty() && p.find(f) == std::string::npos && lower(title).find(f) == std::string::npos) continue;
            std::string nt = pg.ex.boxed ? "service" : "";
            if (pg.deleted) nt = "deleted";
            items.push_back({p, title, nt, pg.ex.tx, pg.versions.size()});
        }
        std::stable_sort(items.begin(), items.end(), [&](const It &a, const It &b) {
            if (sort_key == "t") return a.title < b.title;
            if (sort_key == "tx") return a.tx > b.tx;
            if (sort_key == "v") return a.nv > b.nv;
            if (sort_key == "n") return a.note < b.note;
            return a.p < b.p;
        });
        list_ids.clear();
        rows.clear();
        for (auto &i : items) { list_ids.push_back(i.p); rows.push_back({i.p, i.title, std::to_string(i.tx), std::to_string(i.nv), i.note}); }
    }
    std::vector<std::array<std::string, 5>> rows;
    ImU32 row_colour(int r)
    {
        auto &pg = pages[list_ids[r]];
        if (pg.deleted) return rgb(0x99, 0x99, 0x99);
        if (edited.count(list_ids[r])) return rgb(0x5a, 0xa9, 0xff);
        if (pg.ex.boxed) return ACCENT;
        return FG;
    }
    Page &page() { return pages[cur]; }
    const std::vector<Version> &versions()
    {
        Page &pg = page();
        if (!full) return pg.versions;
        auto it = full_cache.find(cur);
        if (it == full_cache.end()) it = full_cache.emplace(cur, P.full_versions(pg)).first;
        return it->second;
    }
    void open_page(const std::string &p)
    {
        if (!pages.count(p)) { status("there is no page " + p); return; }
        set_flofs();
        cur = p; ver = 0; cursor_r = 1; cursor_c = 0;
        follow = true;
        draw_page();
        screen_focus = true;
    }
    void step(int d)
    {
        if (cur.empty() || ids.empty()) return;
        auto it = std::find(ids.begin(), ids.end(), cur);
        int i = (int)(it - ids.begin());
        i = (i + d + (int)ids.size()) % (int)ids.size();
        open_page(ids[i]);
    }
    void step_ver(int d) { size_t n = versions().size(); if (!n) return; ver = (ver + d + n) % n; draw_page(); }
    Version &snap_mut() { auto &V = page().versions; return V[std::min(ver, V.size() - 1)]; }
    const Version &snap() { auto &V = versions(); return V[std::min(ver, V.size() - 1)]; }
    bool editable() { if (full) { status("the complete page is assembled from versions \xE2\x80\x94 untick \xE2\x80\x9C" "complete page\xE2\x80\x9D to edit"); return false; } return true; }
    Image current_image()
    {
        const Version &s = snap();
        Page &pg = page();
        const Charset &t = P.table(&pg);
        Over x = P.fitted(s.rows, P.overlay(pg, &s), t);
        RenderOpts o;
        o.reveal = reveal; o.flash_on = flash_on; o.tv = tv; o.codes = codes; o.cursor_r = cursor_r; o.cursor_c = cursor_c;
        return render_teletext(s.rows, t, P.table2(), &x, o, &last_flash, &last_box);
    }
    void draw_page(bool keep_flash = false)
    {
        if (cur.empty() || !has_project || page().versions.empty()) return;
        if (!keep_flash) flash_on = true;
        Image im = current_image();
        tex.set(im.px.data(), im.w, im.h);
        flash_at = last_flash ? SDL_GetTicks() / 1000.0 + (flash_on ? 1.0 : 0.333) : 0;
        snprintf(pnum, sizeof pnum, "%s", cur.c_str());
        Page &pg = page();
        for (int i = 0; i < 5; i++) snprintf(keys[i], sizeof keys[i], "%s", i < (int)pg.ex.flof.size() ? pg.ex.flof[i].c_str() : "");
        kpage = cur;
        if (!last_box && tv) status("this page has no boxes \xE2\x80\x94 on a TV screen its text is not shown");
    }
    // editing
    void push() { undo.push_back({cur, page()}); if (undo.size() > 300) undo.erase(undo.begin()); }
    void touch() { full_cache.clear(); dirty = true; edited.insert(cur); status("there are unsaved changes (Ctrl+S saves, Ctrl+Z undoes)"); }
    Row &row(int r) { auto &s = snap_mut(); if (!s.rows.count(r)) { Row x; x.fill(0x20); s.rows[r] = x; } return s.rows[r]; }
    bool set_char(int code)
    {
        if (!editable()) return false;
        if (cursor_r == 0 && cursor_c < 8) { status("the first 8 positions of the header are filled automatically"); return false; }
        push(); row(cursor_r)[cursor_c] = (uint8_t)code; touch(); return true;
    }
    void move(int dr, int dc)
    {
        int r = cursor_r + dr, c = cursor_c + dc;
        if (c > 39) { c = 0; r++; }
        if (c < 0) { c = 39; r--; }
        cursor_r = std::max(0, std::min(24, r)); cursor_c = c;
        draw_page();
    }
    void insert(int code) { if (cur.empty()) return; if (set_char(code)) move(0, 1); screen_focus = true; }
    void on_char(char32_t ch)
    {
        if (cur.empty() || ch < 0x20 || ch == 0x7F) return;
        auto rev = charset_reverse(P.table(&page()));
        auto it = rev.find(ch);
        if (it == rev.end()) { status("the character \xE2\x80\x9C" + from_cp(ch) + "\xE2\x80\x9D is not in this page's character set"); return; }
        if (set_char(it->second)) move(0, 1);
    }
    void action(int i)
    {
        if (cur.empty()) return;
        if (i == 5) { push(); page().deleted = !page().deleted; touch(); fill_list(); draw_page(); return; }
        if (!editable()) return;
        auto &V = page().versions;
        if (i == 0) { if (cursor_r == 0) return; push(); Row x; x.fill(0x20); snap_mut().rows[cursor_r] = x; touch(); draw_page(); }
        else if (i == 1) { if (cursor_r == 0) { status("the header cannot be deleted"); return; } push(); snap_mut().rows.erase(cursor_r); snap_mut().c.erase(cursor_r); touch(); draw_page(); }
        else if (i == 2) {
            ask_text("Row from another version", fmt("Version number (1\xE2\x80\x93%zu) to take row %d from:", V.size(), cursor_r), "1", [this](std::string v) {
                auto &V = page().versions;
                int k = atoi(v.c_str());
                if (k < 1 || k > (int)V.size()) return;
                auto it = V[k - 1].rows.find(cursor_r);
                if (it == V[k - 1].rows.end()) { status("that version does not have this row"); return; }
                Row src = it->second;
                push(); snap_mut().rows[cursor_r] = src; touch(); draw_page();
            });
        } else if (i == 3) {
            if (V.size() < 2) { status("the last version cannot be deleted \xE2\x80\x94 delete the page instead"); return; }
            push(); V.erase(V.begin() + std::min(ver, V.size() - 1)); ver = ver ? ver - 1 : 0; touch(); draw_page();
        } else if (i == 4) { push(); Version c = snap_mut(); V.insert(V.begin() + std::min(ver + 1, V.size()), c); ver++; touch(); draw_page(); }
    }
    void set_flofs() { if (cur.empty()) return; for (int i = 0; i < 5; i++) set_flof(i); }
    void set_flof(int i)
    {
        if (kpage.empty() || !pages.count(kpage)) return;
        std::string v = strip(keys[i]);
        Page &pg = pages[kpage];
        std::vector<std::string> Fl = pg.ex.flof;
        Fl.resize(5);
        if (!v.empty() && !(v.size() == 3 && isdigit((uint8_t)v[0]) && isdigit((uint8_t)v[1]) && isdigit((uint8_t)v[2]) && v[0] >= '1' && v[0] <= '8')) {
            status("page number: three digits, 100\xE2\x80\x93" "899");
            snprintf(keys[i], sizeof keys[i], "%s", Fl[i].c_str());
            return;
        }
        if (Fl[i] == v) return;
        undo.push_back({kpage, pg});
        Fl[i] = v;
        bool any = false;
        for (auto &x : Fl) if (!x.empty()) any = true;
        pg.ex.flof = any ? Fl : std::vector<std::string>();
        dirty = true; edited.insert(kpage); status("there are unsaved changes");
    }
    void do_undo()
    {
        if (undo.empty()) { status("nothing to undo"); return; }
        auto u = undo.back();
        undo.pop_back();
        pages[u.first] = u.second;
        full_cache.clear(); dirty = true; edited.insert(u.first);
        if (u.first != cur) open_page(u.first); else draw_page();
    }
    void save()
    {
        if (cur.empty()) return;
        set_flofs();
        try { P.save_state(pages, edited); dirty = false; status("saved to " + P.rel(P.edited_path) + fmt(" (pages changed: %zu)", edited.size())); }
        catch (std::exception &e) { message("Save", e.what()); }
    }
    void run_export(bool fullp, bool clean)
    {
        if (!edited.empty()) save();
        Pages copy = pages;
        Project pc = P;
        auto res = std::make_shared<std::string>();
        run_task("Export", [copy, pc, fullp, clean, res](Task &t) { { std::lock_guard<std::mutex> l(t.mu); t.step = "exporting"; } *res = pc.export_all(copy, fullp, clean); },
                 [this, res](bool ok) { status(ok ? *res : "export error"); });
    }
    void do_export(bool fullp)
    {
        if (cur.empty()) return;
        if (fullp)
            choose("Export", "Write output_full.t42, output_full.html and the folder html_full/ (complete pages only \xE2\x80\x94 one per subpage) to\n" + P.outdir + "?\nExisting files will be replaced.",
                   {"Export"}, [this](int) { run_export(true, false); });
        else
            choose("Export", "Write output.t42, output.html and the folder html/ to\n" + P.outdir + "\n(existing files will be replaced).\n\nClean the stream as well?",
                   {"Yes \xE2\x80\x94 one assembled copy of every subpage, without repeats and damaged copies", "No \xE2\x80\x94 every version as it was received"},
                   [this](int c) { run_export(false, c == 0); });
    }
    void save_png()
    {
        if (cur.empty()) return;
        Image im = current_image();
        std::string name = basename(stem_path(R->path)) + "_" + cur + fmt("_v%zu.png", ver + 1);
        open_folder_dialog("Folder for the PNG", P.outdir, [this, im, name](std::string d) {
            std::string f = path_join(d, name);
            png_save(f, im.scaled(im.w * 2, im.h * 2));
            status("saved: " + f);
        });
    }
    void do_squash()
    {
        if (cur.empty()) return;
        if (projDir != R->teletext.project) { load(R->teletext.project); return; }
        if (!edited.empty()) save();
        auto res = std::make_shared<std::string>(), out = std::make_shared<std::string>();
        std::string dir = projDir;
        run_task("Clean stream", [dir, res, out](Task &t) {
            Progress pr;
            pr.on_step = [&](const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.step = s; };
            pr.on_log = [&](const std::string &s) { std::lock_guard<std::mutex> l(t.mu); t.log.push_back(s); };
            *out = ttClean(dir, pr, *res);
        }, [this, res, out](bool ok) {
            if (!ok) { status("cleaning failed"); return; }
            if (load(*out)) status("cleaned: " + *res);
        });
    }
    void do_words()
    {
        if (cur.empty()) return;
        choose("Restore words", "Find damaged words (built-in dictionary, words of this recording and the web speller) on:", {"This page (" + cur + ")", "All pages"}, [this](int which) {
            std::set<std::string> one{cur};
            Pages copy = pages;
            std::map<std::string, Page> before;
            Pages orig;
            const Pages *ev = nullptr;
            if (projDir != R->teletext.project) {
                Project po;
                std::set<std::string> e2;
                std::string s2;
                if (po.open(R->teletext.project)) try { orig = po.load_state(e2, s2); ev = &orig; } catch (...) {}
            }
            auto fx = P.restore_words(copy, &before, true, which == 0 ? &one : nullptr, ev);
            if (fx.empty()) { message("Restore words", "No damaged words with a confident replacement were found.\n\n" + P.words_note); return; }
            int n = 0;
            for (auto &f : fx) n += f.n;
            std::string txt = fmt("%d words on %zu pages can be restored:\n\n", n, before.size());
            for (size_t i = 0; i < fx.size() && i < 30; i++) txt += fx[i].page + ":  " + fx[i].from + "  \xE2\x86\x92  " + fx[i].to + (fx[i].n > 1 ? fmt("  (%d)", fx[i].n) : "") + "\n";
            if (fx.size() > 30) txt += fmt("\xE2\x80\xA6 and %zu more\n", fx.size() - 30);
            if (!P.words_note.empty()) txt += "\n" + P.words_note + "\n";
            txt += "\nApply? Every page can be undone (Ctrl+Z).";
            auto cp = std::make_shared<Pages>(copy);
            auto bf = std::make_shared<std::map<std::string, Page>>(before);
            choose("Restore words", txt, {"Apply"}, [this, cp, bf, n](int) {
                for (auto &b : *bf) { undo.push_back({b.first, b.second}); pages[b.first] = (*cp)[b.first]; edited.insert(b.first); }
                while (undo.size() > 300) undo.erase(undo.begin());
                full_cache.clear(); dirty = true; fill_list(); draw_page();
                status(fmt("%d words restored on %zu pages \xE2\x80\x94 there are unsaved changes", n, bf->size()));
            });
        });
    }
    void export_srt()
    {
        if (cur.empty()) return;
        std::vector<std::pair<std::string, int>> c6;
        try { if (exists(P.stream)) c6 = subtitle_pages(P.stream); } catch (...) {}
        std::string hint;
        if (c6.empty()) hint = "The stream has no pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6).";
        else { hint = "Pages with the \xE2\x80\x9Csubtitle\xE2\x80\x9D flag (C6): "; for (size_t i = 0; i < c6.size() && i < 6; i++) hint += (i ? ", " : "") + c6[i].first + fmt(" (%d)", c6[i].second); }
        ask_text("Subtitles .srt", hint + "\n\nSubtitle page number:", c6.empty() ? "888" : c6[0].first, [this](std::string pg) {
            pg = strip(pg);
            for (auto &ch : pg) ch = (char)toupper((uint8_t)ch);
            bool okp = pg.size() == 3 && pg[0] >= '1' && pg[0] <= '8';
            for (size_t i = 1; i < pg.size() && okp; i++) okp = isxdigit((uint8_t)pg[i]);
            if (!okp) { message("Subtitles .srt", "Page number: three characters, the first is the magazine 1\xE2\x80\x93" "8 (e.g. 888)."); return; }
            Project pc = P;
            auto res = std::make_shared<std::string>();
            run_task("Subtitles from page " + pg, [pc, pg, res](Task &) {
                auto r = pc.export_srt(pg);
                *res = r.second ? fmt("subtitles: %d captions from page ", r.second) + pg + " \xE2\x86\x92 " + pc.rel(r.first)
                                : "page " + pg + " has no subtitles (no transmitted pages with the C6 flag) \xE2\x80\x94 an empty " + pc.rel(r.first) + " was written";
            }, [this, res](bool ok) { status(ok ? *res : "subtitle export error"); });
        });
    }

    void flags(int &f) override { f |= ImGuiWindowFlags_MenuBar; }
    void menus()
    {
        if (!ImGui::BeginMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save", "Ctrl+S")) save();
            if (ImGui::MenuItem("Export \xE2\x86\x92 output.t42 + HTML")) do_export(false);
            if (ImGui::MenuItem("Export complete pages only \xE2\x86\x92 output_full.t42 + HTML")) do_export(true);
            if (ImGui::MenuItem("Export subtitles .srt\xE2\x80\xA6")) export_srt();
            if (ImGui::MenuItem("Save page as PNG\xE2\x80\xA6")) save_png();
            if (ImGui::MenuItem("Open export folder") && has_project) open_path(P.outdir);
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) close_asked = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Character set")) {
            for (auto &c : CHARSET_NAMES)
                if (ImGui::MenuItem(c.second.c_str(), nullptr, has_project && c.first == P.charset) && has_project) {
                    P.set_charset(c.first); fill_list(); draw_page();
                    status("character set: " + c.second + " \xE2\x80\x94 saved in the project; run Export to update the HTML");
                }
            ImGui::Separator();
            ImGui::TextDisabled("Second set (ESC code):");
            for (auto &c : CS2)
                if (ImGui::MenuItem(("   " + c.second).c_str(), nullptr, has_project && c.first == P.charset2) && has_project) {
                    P.set_charset2(c.first); fill_list(); draw_page();
                    status("second set (ESC): " + c.second + " \xE2\x80\x94 saved in the project");
                }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Clean stream (squash) \xE2\x86\x92 clean.t42")) do_squash();
            if (ImGui::MenuItem("Restore words\xE2\x80\xA6")) do_words();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    bool close_asked = false;

    void screen(ImVec2 avail)
    {
        ImVec2 at, sz;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, BG);
        ImGui::BeginChild("##scr", avail, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
        image_fit(tex, ImGui::GetContentRegionAvail(), 1.0, &at, &sz);
        if (screen_focus) DL()->AddRect(at, ImVec2(at.x + sz.x, at.y + sz.y), ACCENT);
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
            ImVec2 m = ImGui::GetMousePos();
            if (m.x >= at.x && m.y >= at.y && m.x < at.x + sz.x && m.y < at.y + sz.y && has_project) {
                cursor_r = std::min(24, (int)((m.y - at.y) * 25 / sz.y));
                cursor_c = std::min(39, (int)((m.x - at.x) * 40 / sz.x));
                draw_page();
            }
            screen_focus = true;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    void keyboard()
    {
        ImGuiIO &io = ImGui::GetIO();
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) save();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !ImGui::IsAnyItemActive()) do_undo();
        if (!screen_focus || ImGui::IsAnyItemActive() || io.KeyCtrl || cur.empty()) return;
        auto k = [](ImGuiKey x) { return ImGui::IsKeyPressed(x, true); };
        if (k(ImGuiKey_LeftArrow)) move(0, -1);
        if (k(ImGuiKey_RightArrow)) move(0, 1);
        if (k(ImGuiKey_UpArrow)) move(-1, 0);
        if (k(ImGuiKey_DownArrow)) move(1, 0);
        if (k(ImGuiKey_Home)) { cursor_c = 0; draw_page(); }
        if (k(ImGuiKey_End)) { cursor_c = 39; draw_page(); }
        if (k(ImGuiKey_Enter)) { cursor_r = std::min(24, cursor_r + 1); cursor_c = 0; draw_page(); }
        if (k(ImGuiKey_PageUp)) step(-1);
        if (k(ImGuiKey_PageDown)) step(1);
        if (k(ImGuiKey_Backspace)) { move(0, -1); set_char(0x20); draw_page(); }
        if (k(ImGuiKey_Delete)) { set_char(0x20); draw_page(); }
        for (ImWchar c : io.InputQueueCharacters) on_char(c);
        io.InputQueueCharacters.resize(0);
    }
    void page_tab()
    {
        if (ImGui::Button("\xE2\x97\x80 page", ImVec2(px(72), 0))) step(-1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(52));
        if (ImGui::InputText("##pn", pnum, sizeof pnum, ImGuiInputTextFlags_EnterReturnsTrue)) open_page(strip(pnum));
        ImGui::SameLine();
        if (ImGui::Button("page \xE2\x96\xB6", ImVec2(px(72), 0))) step(1);
        ImGui::SameLine(0, px(16));
        if (ImGui::Button("\xE2\x97\x80##v", ImVec2(px(34), 0))) step_ver(-1);
        ImGui::SameLine();
        if (has_project && !cur.empty()) {
            Page view = page();
            view.versions = versions();
            ImGui::TextUnformatted(P.version_label(view, std::min(ver, view.versions.size() - 1)).c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x96\xB6##v", ImVec2(px(34), 0))) step_ver(1);
        if (ImGui::Checkbox("concealed text", &reveal)) draw_page();
        ImGui::SameLine();
        if (ImGui::Checkbox("complete page", &full)) { ver = 0; draw_page(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("as on a TV screen", &tv)) draw_page();
        ImGui::SameLine();
        if (ImGui::Checkbox("show control codes", &codes)) draw_page();
        if (has_project && !cur.empty()) {
            Page &pg = page();
            std::string nt = P.service_note(pg);
            if (pg.deleted) nt = "page deleted (will not be exported)";
            std::string sv = P.service_line();
            if (!sv.empty()) nt = nt.empty() ? sv : nt + "  \xC2\xB7  " + sv;
            if (!nt.empty()) wrapped(ACCENT, nt);
        }
        float bottom = px(34) * 4 + px(50);
        screen(ImVec2(ImGui::GetContentRegionAvail().x, std::max(px(200), ImGui::GetContentRegionAvail().y - bottom)));
        // colour keys
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Colour keys (FLOF):");
        for (int i = 0; i < 5; i++) {
            ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, KEYC[i]);
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(0, 0, 0));
            ImGui::SetNextItemWidth(px(52));
            if (ImGui::InputText("##k", keys[i], sizeof keys[i], ImGuiInputTextFlags_EnterReturnsTrue)) set_flof(i);
            if (ImGui::IsItemDeactivatedAfterEdit()) set_flof(i);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { set_flof(i); std::string v = strip(keys[i]); if (!v.empty()) open_page(v); }
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("double-click \xE2\x80\x94 go to");
        static const char *an[6] = {"Clear row", "Delete row", "Row from another version\xE2\x80\xA6", "Delete version", "Copy version", "Delete / restore page"};
        for (int i = 0; i < 6; i++) { if (i) ImGui::SameLine(); if (ImGui::Button(an[i])) action(i); }
        for (int i = 0; i < 14; i++) {
            if (i) ImGui::SameLine(0, i == 7 ? px(16) : px(4));
            ImGui::PushID(100 + i);
            ImGui::PushStyleColor(ImGuiCol_Button, TT[i % 7 + 1]);
            ImGui::PushStyleColor(ImGuiCol_Text, i % 7 + 1 == 4 || i % 7 + 1 == 1 ? rgb(255, 255, 255) : rgb(0, 0, 0));
            if (ImGui::Button(i < 7 ? "T" : "M", ImVec2(px(28), 0))) insert(i < 7 ? i + 1 : 0x10 + i - 6);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
            if (i == 6) { ImGui::SameLine(); ImGui::TextUnformatted("text"); }
            if (i == 13) { ImGui::SameLine(); ImGui::TextUnformatted("mosaic"); }
        }
        static const char *pn[7] = {"Background = colour", "Black background", "Double height", "Normal height", "Contiguous mosaic", "Separated mosaic", "Concealed"};
        static const int pc[7] = {0x1d, 0x1c, 0x0d, 0x0c, 0x19, 0x1a, 0x18};
        for (int i = 0; i < 7; i++) { if (i) ImGui::SameLine(); if (ImGui::Button(pn[i])) insert(pc[i]); }
        if (has_project && !cur.empty()) {
            Page &pg = page();
            const Version &s = snap();
            auto r = s.rows.find(cursor_r);
            wrapped(MUTED, fmt("row %d, column %d", cursor_r, cursor_c) + (r != s.rows.end() ? fmt(", code 0x%02x", r->second[cursor_c]) : ", no row") +
                               fmt(" \xC2\xB7 received %d times, subpages %d", pg.ex.tx, pg.ex.subpages) + (s.t > 0 ? "  \xC2\xB7  " + ui::rec_time(s.t) + " into the recording" : ""));
        }
        wrapped(DIM, "Click a character and type. Arrows \xE2\x80\x94 move, Backspace/Delete \xE2\x80\x94 erase, Enter \xE2\x80\x94 next row, PageUp/PageDown \xE2\x80\x94 neighbouring pages.");
    }
    void quality_tabv()
    {
        if (!quality.is_obj()) { wrapped(MUTED, "No quality map: it is built when the pages come from a teletext VBI recording."); return; }
        header("Tape quality per minute of " + P.name, "(broadcast time in brackets)");
        auto &mins = quality["minutes"].a;
        static const char *titles[3] = {"Teletext lines not received, %", "Of these unreadable (signal present), %", "Parity errors, % of bytes"};
        float ch = std::max(px(160), ImGui::GetContentRegionAvail().y / 3);
        Rc area = take(ch);
        float gap = px(12), cw = (area.w() - 2 * gap) / 3;
        ImVec2 mp = ImGui::GetMousePos();
        for (int k = 0; k < 3; k++) {
            Rc r{area.l + k * (cw + gap), area.t + px(22), area.l + k * (cw + gap) + cw, area.b};
            text(r.l, area.t, titles[k], FG);
            fillr(r, PANEL);
            std::vector<double> v;
            for (auto &d : mins) v.push_back(k == 0 ? round((100 - d["received"].num()) * 100) / 100 : k == 1 ? d["unreadable"].num() : d["parity"].num());
            double hi = 0;
            for (double x : v) hi = std::max(hi, x);
            hi = hi > 0 ? hi * 1.15 : 1;
            float L = px(40), B = px(24), T = px(10), Rr = px(8);
            auto Y = [&](double x) { return r.t + T + (float)((r.h() - T - B) * (1 - x / hi)); };
            for (int i = 0; i <= 4; i++) {
                float yy = Y(hi * i / 4);
                fillr({r.l + L, yy, r.r - Rr, yy + 1}, LINE);
                text(r.l + L - 4, yy - px(7), hi < 2 ? fmt("%.2f", hi * i / 4) : fmt("%.1f", hi * i / 4), MUTED, NORM, 8, RIGHT);
            }
            size_t n = v.size();
            if (!n) continue;
            float bw = (r.w() - L - Rr) / n;
            for (size_t i = 0; i < n; i++) {
                float x0 = r.l + L + i * bw + 1, x1 = std::max(x0 + 1, r.l + L + (i + 1) * bw - 1);
                fillr({x0, Y(v[i]), x1, r.b - B}, ACCENT);
                if (n <= 24 || i % ((n + 23) / 24) == 0) text((x0 + x1) / 2, r.b - B + 2, std::to_string(mins[i]["minute"].integer()), MUTED, NORM, 8, CENTER);
                if (mp.x >= x0 && mp.x < x1 && mp.y >= r.t && mp.y < r.b) ImGui::SetTooltip("minute %d (%s): %.3f", mins[i]["minute"].integer(), mins[i].get_str("air").c_str(), v[i]);
            }
        }
        if (ImGui::BeginTable("##q", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, ImGui::GetContentRegionAvail().y - px(44)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const char *c : {"Recording minute", "Broadcast", "Lines received, %", "Unreadable, %", "Parity errors, %"}) ImGui::TableSetupColumn(c);
            ImGui::TableHeadersRow();
            for (auto &d : mins) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%d", (int)d["minute"].integer());
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(d.get_str("air").c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", d["received"].num());
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f", d["unreadable"].num());
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.3f", d["parity"].num());
            }
            ImGui::EndTable();
        }
        wrapped(DIM, "Received \xE2\x80\x94 share of teletext VBI lines that yielded a packet; unreadable \xE2\x80\x94 signal present but no decoder got a packet; errors \xE2\x80\x94 bytes with bad parity in received packets.");
    }
    void draw() override
    {
        menus();
        if (close_asked) {
            close_asked = false;
            if (!cur.empty()) set_flofs();
            if (dirty) choose("Unsaved edits", "Save the edited pages?", {"Save", "Do not save"}, [this](int c) { if (c == 0) save(); open = false; });
            else open = false;
        }
        if (flash_at > 0 && SDL_GetTicks() / 1000.0 >= flash_at) { flash_on = !flash_on; draw_page(true); }
        keyboard();
        if (ImGui::Button("Save (Ctrl+S)")) save();
        ImGui::SameLine(); if (ImGui::Button("Export \xE2\x86\x92 output.t42 + HTML")) do_export(false);
        ImGui::SameLine(); if (ImGui::Button("Undo (Ctrl+Z)")) do_undo();
        ImGui::SameLine();
        if (ImGui::Button("Open html/index.html") && has_project) {
            std::string p = path_join(path_join(P.outdir, "html"), "index.html");
            if (exists(p)) open_path(p); else status("html/index.html does not exist yet \xE2\x80\x94 run Export first");
        }
        ImGui::SameLine(); if (ImGui::Button(cleaned ? "Show as received" : "Clean stream")) do_squash();
        ImGui::SameLine(); if (ImGui::Button("Restore words\xE2\x80\xA6")) do_words();
        ImGui::SameLine(); ImGui::PushStyleColor(ImGuiCol_Text, MUTED); ImGui::TextUnformatted(msg.c_str()); ImGui::PopStyleColor();
        ImVec2 av = ImGui::GetContentRegionAvail();
        float lw = std::min(std::max(px(300), av.x * 0.36f), av.x - px(500));
        ImGui::BeginChild("##left", ImVec2(lw, av.y), ImGuiChildFlags_ResizeX);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##s", "search by number or title", search, sizeof search)) { if (has_project) fill_list(); }
        if (ImGui::BeginTable("##pages", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Page", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, px(50));
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Received", ImGuiTableColumnFlags_WidthFixed, px(66));
            ImGui::TableSetupColumn("Versions", ImGuiTableColumnFlags_WidthFixed, px(60));
            ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed, px(70));
            ImGui::TableHeadersRow();
            if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs())
                if (ss->SpecsDirty && ss->SpecsCount) {
                    static const char *k[5] = {"p", "t", "tx", "v", "n"};
                    sort_key = k[ss->Specs[0].ColumnIndex];
                    if (has_project) fill_list();
                    ss->SpecsDirty = false;
                }
            int selrow = -1;
            for (size_t i = 0; i < list_ids.size(); i++) if (list_ids[i] == cur) selrow = (int)i;
            ImGuiListClipper clip;
            clip.Begin((int)rows.size());
            if (follow && selrow >= 0) clip.IncludeItemByIndex(selrow);
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                    ImGui::TableNextRow();
                    ImGui::PushStyleColor(ImGuiCol_Text, row_colour(r));
                    for (int c = 0; c < 5; c++) {
                        ImGui::TableSetColumnIndex(c);
                        if (c == 0) {
                            ImGui::PushID(r);
                            if (ImGui::Selectable(rows[r][0].c_str(), r == selrow, ImGuiSelectableFlags_SpanAllColumns) && list_ids[r] != cur) open_page(list_ids[r]);
                            ImGui::PopID();
                            if (follow && r == selrow) ImGui::SetScrollHereY(0.5f);
                        } else
                            ImGui::TextUnformatted(rows[r][c].c_str());
                    }
                    ImGui::PopStyleColor();
                }
            follow = false;
            ImGui::EndTable();
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) screen_focus = false;
        ImGui::EndChild();
        ImGui::SameLine(0, px(10));
        ImGui::BeginChild("##right", ImVec2(0, av.y));
        if (quality_tab ? ImGui::Button("Page", ImVec2(px(90), 0)) : accent_button("Page", ImVec2(px(90), 0))) quality_tab = false;
        ImGui::SameLine();
        if (quality_tab ? accent_button("Tape quality", ImVec2(px(120), 0)) : ImGui::Button("Tape quality", ImVec2(px(120), 0))) quality_tab = true;
        if (quality_tab) quality_tabv(); else page_tab();
        ImGui::EndChild();
    }
};
}  // namespace

void teletext_window(const ResultPtr &r)
{
    auto w = std::make_unique<TtWin>();
    w->R = r;
    w->title = "Teletext \xE2\x80\x94 " + ui::file_name(*r) + "##tt" + std::to_string((intptr_t)r.get());
    w->size = ImVec2(px(1320), px(920));
    if (!w->load(r->teletext.project)) w->status("the teletext project is missing: " + r->teletext.project);
    add_window(std::move(w));
}
}
