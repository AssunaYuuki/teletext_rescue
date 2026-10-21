#pragma once
// "squash" of VHSTTX 2.7 (teletext/pipeline.py, vhs-teletext with KOTYA8's modes): a .t42 stream cut into
// subpages (a header closes its magazine's previous page), the copies of each page grouped, every group of at least
// minDuplicates copies made into one subpage by the most common byte of each place.
//   V3: copies with the same subcode (and, with threshold >= 0, fewer than threshold differing bytes) are one group
//   V1: copies whose shown characters agree enough row by row, regrouped around the groups' squashed pages
//   Auto: V1 or V3 per page by how well the groups agree, rows taken from the other grouping where they read better
//   Custom: weighted header / body / footer similarity with a subcode bonus (profiles: balanced, aggressive,
//   conservative, broken-subcodes)
#include <cstdint>
#include <string>
#include <vector>

enum class SquashMode { V1, V3, Auto, Custom };

struct SquashProfile {
    double matchThreshold = 0.74, headerWeight = 0.55, bodyWeight = 1.0, footerWeight = 0.45;
    double subcodeMatchBonus = 0.12, subcodeMismatchPenalty = 0.04;
    int iterations = 3;
};
// balanced (default), aggressive, conservative, broken-subcodes; false for another name
bool squashProfileByName(const std::string& name, SquashProfile& out);

struct SquashOptions {
    SquashMode mode = SquashMode::V3;
    SquashProfile profile;
    int minDuplicates = 3;
    int threshold = -1;       // V3 / Auto: most bytes two copies may differ by (-1 = any)
    int v1Iterations = 3;
};

// t42: 42 bytes a packet. Returns the squashed stream; note says how many pages / groups / subpages.
std::vector<uint8_t> ttSquash(const std::vector<uint8_t>& t42, const SquashOptions& o, std::string& note);
// "v1", "v3", "auto", "custom", or a profile name (custom with that profile); false when unknown
bool squashOptionsByName(const std::string& name, SquashOptions& o);
