// arjmerge out.arj a.arj b.arj [...] [-x dir]: receptions of the same file (Бизнес-Текст) made into one by
// combineCopies (src/arjcheck.cpp), the archive checked and unpacked
#include "arjcheck.h"
#include <cstdio>
#include <string>
#include <vector>

static std::vector<uint8_t> load(const char* p)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(p, "rb");
    if (!f) return d;
    int c;
    while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c);
    fclose(f);
    return d;
}

int main(int argc, char** argv)
{
    if (argc < 4) { printf("arjmerge out.arj a.arj b.arj [...] [-x dir]\n"); return 1; }
    std::vector<std::vector<uint8_t>> copies;
    std::string xdir;
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "-x" && i + 1 < argc) { xdir = argv[++i]; continue; }
        copies.push_back(load(argv[i]));
    }
    std::string log;
    auto M = combineCopies(copies, log);
    printf("%s", log.c_str());
    if (M.empty()) return 1;
    FILE* f = fopen(argv[1], "wb");
    if (f) { fwrite(M.data(), 1, M.size(), f); fclose(f); }
    ArjCheck c = arjCheck(M);
    for (auto& e : c.files) printf("  %-14s %7u of %7u bytes  %s\n", e.name.c_str(), e.got, e.size, e.ok ? "whole (CRC right)" : e.why.c_str());
    if (!xdir.empty()) printf("%s", arjExtract(c, xdir).c_str());
    return 0;
}
