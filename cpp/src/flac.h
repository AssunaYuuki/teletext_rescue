// VBI recordings compressed with FLAC (.vbi.flac, .flac): FLAC here is lossless byte compression, not sound.
// One channel, 8 or 16 bits per sample; signed samples are shifted back to unsigned (as such files are made).
// Ported from the original Teletext Rescue (flac.cpp).
#pragma once
#include "util.h"

bool is_flac(const std::string &path);                       // "fLaC" at the start of the file
// unpack to a raw file; returns the byte count. Frames are decoded in parallel. comment = Vorbis comments, one per line.
uint64_t flac_to_raw(const std::string &in, const std::string &out, Progress &pr, std::string *comment = nullptr);
// The unpacked copy lives next to the FLAC: x.vbi.flac -> x_vbi/x.vbi (as in the original program),
// so a recording is unpacked once and opened ready the next time.
std::string flac_cache_path(const std::string &flac);
bool flac_cached(const std::string &flac);                   // the copy exists and has the full size
// returns the raw path; again = unpack even if a ready copy exists
std::string flac_unpack_cached(const std::string &flac, bool again, Progress &pr);
// capture notes saved with an unpacked copy (FLAC comment, e.g. "2048x32@25Hz"); empty if none
std::string capture_notes(const std::string &raw);
