// test: src/nabts/nabts_fec.h on synthetic NABTS FEC datacasts (RFC 2728): bundles of N data rows with their row
// codes and two check rows, sent in the layouts the decoder must learn (check rows after their data, as RFC 2728
// sends them; before, as WaveTop does; one check row never sent), then damaged - one byte wrong in a row, rows with
// two bytes wrong, packets lost - and decoded. Every data row the decoder calls good must equal the row sent.
// build: g++ -std=gnu++20 -O2 tools/nabtsfec/main.cpp -Isrc/nabts -o build/nabtsfec.exe
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
// Hamming 8/4 of ETSI EN 300 706 §8.2 (the tables src/nabts/teletext.cpp makes): exact and one bit put right
namespace ham {
int dec[256], fix[256];
uint8_t enc[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA};
static struct Init {
    Init()
    {
        for (int x = 0; x < 256; x++) {
            dec[x] = fix[x] = -1;
            for (int d = 0; d < 16; d++) {
                if (enc[d] == x) dec[x] = fix[x] = d;
                else if (__builtin_popcount(enc[d] ^ x) == 1) fix[x] = d;
            }
        }
    }
} init;
}
#include "nabts_fec.h"

using namespace nabtsfec;
using FRow = nabtsfec::Row;

struct Sent { FRow row; bool check; };

// the two code bytes of a row (codeword positions 0, 1) so its sums are zero
static void rowCode(FRow &r)
{
    int cw[28];
    toCodeword(r.data(), cw);
    fill(cw, 28, 0, 1);
    fromCodeword(cw, r.data());
}

int main(int argc, char **argv)
{
    std::mt19937 rng(1234);
    int failures = 0;
    struct Case { const char *name; int data; bool lead; bool dropSecondCheck; };
    const Case cases[] = {{"RFC 2728: 14 data rows, check rows after", 14, false, false},
                          {"WaveTop: 16 data rows, check rows before", 16, true, false},
                          {"16 data rows, one check row never sent", 16, false, true}};
    for (auto &cs : cases) {
        const int bundles = 300, P = cs.data + 2;
        std::vector<std::vector<FRow>> truth(bundles);
        std::vector<Sent> stream;
        for (int b = 0; b < bundles; b++) {
            std::vector<FRow> rows(P);
            for (int q = 2; q < P; q++) { for (int i = 0; i < 26; i++) rows[q][i] = (uint8_t)rng(); rowCode(rows[q]); }
            for (int j = 0; j < 28; j++) {   // check rows: each column a codeword (positions 0, 1)
                int col[kMaxPositions];
                for (int q = 0; q < P; q++) col[q] = rows[q][j];
                fill(col, P, 0, 1);
                rows[0][j] = (uint8_t)col[0]; rows[1][j] = (uint8_t)col[1];
            }
            truth[b] = rows;
            auto checks = [&] { stream.push_back({rows[0], true}); if (!cs.dropSecondCheck) stream.push_back({rows[1], true}); else stream.push_back({FRow{}, true}); };
            if (cs.lead) checks();
            for (int q = 2; q < P; q++) stream.push_back({rows[q], false});
            if (!cs.lead) checks();
        }
        // packets: prefix = address 5A3, continuity index, PS 8 (data + 2-byte code) or 12 (check row); damage
        Bytes t33;
        std::vector<int> field;
        int ci = 0, oneByte = 0, twoBytes = 0, lost = 0;
        std::uniform_int_distribution<int> pct(0, 99), col(0, 27);
        for (size_t k = 0; k < stream.size(); k++) {
            int myCi = ci++ % 16;
            bool neverSent = cs.dropSecondCheck && stream[k].check && ((cs.lead && k % P == 1) || (!cs.lead && k % P == (size_t)P - 1));
            if (neverSent) continue;   // the index still counts it: 'M'
            if (pct(rng) < 3) { lost++; continue; }
            FRow r = stream[k].row;
            int roll = pct(rng);
            if (roll < 10) { r[col(rng)] ^= (uint8_t)(1 + rng() % 255); oneByte++; }
            else if (roll < 13) { int a = col(rng), b = (a + 1 + rng() % 27) % 28; r[a] ^= (uint8_t)(1 + rng() % 255); r[b] ^= (uint8_t)(1 + rng() % 255); twoBytes++; }
            uint8_t p[33];
            p[0] = ham::enc[5]; p[1] = ham::enc[0xA]; p[2] = ham::enc[3]; p[3] = ham::enc[myCi]; p[4] = ham::enc[stream[k].check ? 12 : 8];
            for (int i = 0; i < 28; i++) p[5 + i] = r[i];
            t33.insert(t33.end(), p, p + 33);
            field.push_back((int)k);
        }
        if (argc > 1 && &cs == &cases[0]) {   // the RFC 2728 case for a test through the program: <dir>/rfc.t33 and the data sent
            std::string d = argv[1];
            if (FILE *f = fopen((d + "/rfc.t33").c_str(), "wb")) { fwrite(t33.data(), 1, t33.size(), f); fclose(f); }
            if (FILE *f = fopen((d + "/rfc_sent.bin").c_str(), "wb")) {
                for (auto &b : truth) for (int q = 2; q < P; q++) fwrite(b[q].data(), 1, 26, f);
                fclose(f);
            }
        }
        auto ch = decode(t33.data(), t33.size() / 33, field);
        printf("%s\n  sent %zu packets: %d lost, %d one byte wrong, %d two bytes wrong\n", cs.name, stream.size(), lost, oneByte, twoBytes);
        if (ch.size() != 1) { printf("  FAIL: %zu channels\n", ch.size()); failures++; continue; }
        auto &c = ch[0];
        printf("  %s\n", c.summary().c_str());
        if (c.layoutData != cs.data || c.checksLead != cs.lead) { printf("  FAIL: layout %d %s\n", c.layoutData, c.checksLead ? "lead" : "close"); failures++; }
        // every row the decoder calls good equals the row sent at its place (bundles matched by their first good row)
        int wrong = 0, goodRows = 0, whole = 0, usable = 0;
        std::map<std::string, int> where;
        for (int b = 0; b < bundles; b++) for (int q = 2; q < P; q++) where[std::string((const char *)truth[b][q].data(), 28)] = b * 100 + q;
        for (auto &bu : c.bundles) {
            whole += bu.whole();
            usable += bu.usable();
            int bt = -1;
            for (int q = 2; q < P && bt < 0; q++) if (bu.state[q] <= Corrected) { auto it = where.find(std::string((const char *)bu.rows[q].data(), 28)); if (it != where.end()) bt = it->second / 100; }
            if (bt < 0) continue;
            for (int q = 2; q < P; q++) {
                if (bu.state[q] > Rebuilt) continue;
                goodRows++;
                if (bu.rows[q] != truth[bt][q]) {
                    wrong++;
                    int same = 0, st[6] = {};
                    for (int k = 2; k < P; k++) { same += bu.rows[k] == truth[bt][k]; st[bu.state[k]]++; }
                    auto it = where.find(std::string((const char *)bu.rows[q].data(), 28));
                    printf("    wrong: position %d state %d; bundle rows equal to bundle %d: %d of %d; states R%d C%d B%d U%d D%d M%d; row is %s\n", q,
                           bu.state[q], bt, same, P - 2, st[0], st[1], st[2], st[3], st[4], st[5],
                           it == where.end() ? "no row sent" : ("row " + std::to_string(it->second % 100) + " of bundle " + std::to_string(it->second / 100)).c_str());
                }
            }
        }
        printf("  bundles %zu, whole %d, usable with a check of their own %d; data rows called good %d, of them wrong %d\n", c.bundles.size(), whole,
               usable, goodRows, wrong);
        if (wrong) { printf("  FAIL: rows called good but wrong\n"); failures++; }
        if (usable < bundles * (cs.dropSecondCheck ? 7 : 8) / 10) { printf("  FAIL: too few whole bundles\n"); failures++; }
    }
    printf(failures ? "FAILED %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
