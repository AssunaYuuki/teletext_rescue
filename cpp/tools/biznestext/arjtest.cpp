// arjtest <file.arj>...: arjCheck (src/arjcheck.cpp) for each file, to compare with "7z t"
#include "arjcheck.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { printf("%s: cannot open\n", argv[i]); continue; }
        std::vector<uint8_t> d;
        int c;
        while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c);
        fclose(f);
        ArjCheck r = arjCheck(d);
        printf("%s: %s\n", argv[i], r.summary().c_str());
        for (auto& e : r.files)
            printf("  %-14s method %d  %7u -> %7u  got %7u  %s\n", e.name.c_str(), e.method, e.packed, e.size, e.got, e.ok ? "ok" : e.why.c_str());
        if (i + 1 < argc && std::string(argv[i + 1]) == "-x") {   // arjtest file.arj -x outdir
            printf("%s", arjExtract(r, argv[i + 2]).c_str());
            i += 2;
        }
    }
}
