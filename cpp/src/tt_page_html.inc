// A teletext page as plain HTML, as vhs-teletext writes it: every row a line of text in the
// teletext font (teletext2.ttf, double height teletext4.ttf), colours as classes f0..f7 / b0..b7,
// mosaics as the font's block characters (U+EE00 + code, separated U+EE00 + code - 0x20), page
// numbers as links. No scripts, no controls: just the page (one block per version).
// Included by src/nabts/project.cpp and src/legacy/src/project.cpp (each in its own namespace).

static void tt_html_char(std::string &o, const std::string &t)
{
    for (char ch : t) {
        if (ch == '<') o += "&lt;";
        else if (ch == '>') o += "&gt;";
        else if (ch == '&') o += "&amp;";
        else o += ch;
    }
}

static std::string tt_page_html(const Project &P, const Page &pg, const std::string &pid, const std::set<std::string> &exist)
{
    const Charset &t = P.table(&pg);
    std::string body;
    for (size_t k = 0; k < pg.versions.size(); k++) {
        const Version &s = pg.versions[k];
        Over x = P.fitted(s.rows, P.overlay(pg, &s), t);
        auto cells = level1_cells(s.rows, t, P.table2(), &x);
        std::map<int, std::vector<const Cell *>> grid;
        for (auto &c : cells) {
            auto &g = grid[c.r];
            if (g.empty()) g.assign(40, nullptr);
            if (c.c >= 0 && c.c < 40) g[c.c] = &c;
        }
        body += "<div class=\"subpage\" id=\"" + (s.s.empty() ? fmt("%04zu", k) : s.s) + "\">";
        for (int r = 0; r < 25; r++) {
            auto it = grid.find(r);
            bool covered = r > 0 && grid.count(r - 1) && std::any_of(grid[r - 1].begin(), grid[r - 1].end(), [](const Cell *c) { return c && c->bh == 2; });
            if (covered) continue;   // the lower half of a double-height row
            body += "<span class=\"row\">";
            if (it == grid.end()) { body += "<span class=\"f7 b0 nx\">" + std::string(40, ' ') + "</span></span>"; continue; }
            // the characters of the row, and which columns are page numbers to link
            std::vector<std::string> ch(40, " ");
            std::vector<std::string> cls(40, "f7 b0");
            int fg = 7, bg = 0;
            for (int c = 0; c < 40; c++) {
                const Cell *cl = it->second[c];
                if (cl) { fg = cl->fg; bg = cl->bg; }
                std::string k2 = fmt("f%d b%d", fg, bg);   // classes as vhs-teletext's PrinterHTML
                if (cl && cl->h == 2) k2 += " dh";
                if (cl && cl->flash) k2 += " fl";
                if (cl && cl->conceal) k2 += " cn";
                k2 += cl && cl->box ? " bx" : " nx";
                cls[c] = k2;
                if (!cl) continue;
                if (cl->mosaic) {
                    char32_t u = 0xEE00 + (cl->sep ? cl->mosaic - 0x20 : cl->mosaic);
                    ch[c] = from_cp(u);
                } else if (!cl->text.empty()) ch[c] = cl->text;
            }
            std::vector<int> link(40, 0);   // 1 = first digit of a linked number
            std::vector<std::string> flink(40);   // row 24 with FLOF keys: red/green/yellow/cyan text links to them
            bool fast = r == 24 && pg.ex.flof.size() >= 4;
            if (fast)
                for (int c = 0; c < 40; c++) {
                    const char *col = "1236";
                    size_t f = cls[c].size() > 1 ? std::string(col).find(cls[c][1]) : std::string::npos;
                    if (f != std::string::npos && !pg.ex.flof[f].empty() && exist.count(pg.ex.flof[f])) flink[c] = pg.ex.flof[f];
                }
            for (int c = 0; c + 3 <= 40 && !fast; c++) {
                auto dig = [&](int i) { return i >= 0 && i < 40 && ch[i].size() == 1 && ch[i][0] >= '0' && ch[i][0] <= '9'; };
                if (dig(c) && dig(c + 1) && dig(c + 2) && !dig(c - 1) && !dig(c + 3) && ch[c][0] >= '1' && ch[c][0] <= '8' &&
                    exist.count(ch[c] + ch[c + 1] + ch[c + 2]) && ch[c] + ch[c + 1] + ch[c + 2] != pid && cls[c] == cls[c + 2])
                    link[c] = 1;
            }
            for (int c = 0; c < 40;) {
                int e = c;
                while (e < 40 && cls[e] == cls[c]) e++;
                body += "<span class=\"" + cls[c] + "\">";
                if (!flink[c].empty()) body += "<a href=\"" + flink[c] + ".html\">";
                for (int i = c; i < e; i++) {
                    if (link[i] && i + 3 <= e) {
                        body += "<a href=\"" + ch[i] + ch[i + 1] + ch[i + 2] + ".html\">" + ch[i] + ch[i + 1] + ch[i + 2] + "</a>";
                        i += 2;
                    } else tt_html_char(body, ch[i]);
                }
                if (!flink[c].empty()) body += "</a>";
                body += "</span>";
                c = e;
            }
            body += "</span>";
        }
        body += "</div>";
    }
    return "<html>\n    <head>\n        <meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n        <title>Page " + pid +
           "</title>\n        <link rel=\"stylesheet\" type=\"text/css\" href=\"teletext.css\"/>\n    </head>\n    <body>\n    " + body +
           "\n    </body>\n</html>\n";
}
